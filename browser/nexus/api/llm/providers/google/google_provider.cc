#include "custom_browser/browser/nexus/api/llm/providers/google/google_provider.h"

#include "custom_browser/browser/nexus/nexus_secret_crypt.h"

#include <optional>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/sequenced_task_runner.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"

namespace nexus {

namespace {

constexpr size_t kMaxResponseSizeBytes =
    network::SimpleURLLoader::kMaxBoundedStringDownloadSize;

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("nexus_google_llm_provider", R"(
      semantics {
        sender: "Nexus AI Agent Platform"
        description:
          "Sends requests to the Google AI (Gemini) API to generate text "
          "completions and function calls for the AI agent platform."
        trigger:
          "User interacts with an AI agent configured to use the Google AI "
          "provider."
        data:
          "Conversation messages, tool definitions, and generation "
          "parameters. Content depends on the user's conversation."
        destination: GOOGLE_OWNED_SERVICE
      }
      policy {
        cookies_allowed: NO
        setting:
          "Users configure the Google AI provider with their own API key "
          "through the Nexus agent platform settings."
      })");

}  // namespace

// ---------------------------------------------------------------------------
// GoogleProvider
// ---------------------------------------------------------------------------

GoogleProvider::GoogleProvider(
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory)
    : url_loader_factory_(std::move(url_loader_factory)) {
  DETACH_FROM_SEQUENCE(sequence_checker_);
}

GoogleProvider::~GoogleProvider() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

std::string GoogleProvider::GetId() const {
  return "google";
}

std::string GoogleProvider::GetDisplayName() const {
  return "Google AI";
}

LlmProviderType GoogleProvider::GetProviderType() const {
  return LlmProviderType::kCloud;
}

std::vector<ModelInfo> GoogleProvider::GetSupportedModels() const {
  return fetched_models_;
}

std::string GoogleProvider::GetDefaultModelId() const {
  // Default-model selection is owned by the TS layer (nexus_web).
  return std::string();
}

bool GoogleProvider::SupportsStreaming() const {
  return true;
}

bool GoogleProvider::SupportsTools() const {
  return true;
}

bool GoogleProvider::IsConfigured() const {
  return !api_key_.empty();
}

bool GoogleProvider::Configure(const base::DictValue& config) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  ConfigureUserAgent(config);

  const std::string* api_key = config.FindString("api_key");
  if (!api_key || api_key->empty()) {
    DLOG(ERROR) << "Google AI provider requires a non-empty api_key";
    return false;
  }
  api_key_ = NexusSecretCrypt::GetInstance()->Decrypt(*api_key);
  // A non-empty stored value that decrypts to nothing is an UNOPENABLE
  // ciphertext (settings.json copied to another machine / OS user, keystore
  // reset, or the encryptor not ready yet). Report "not configured" rather
  // than sending every request with an empty key — mirrors OpenAI/Anthropic,
  // whose Configure returns IsConfigured().
  if (api_key_.empty()) {
    LOG(ERROR) << "Google AI provider: the stored api_key could not be "
                  "decrypted; treating the provider as unconfigured";
    return false;
  }

  const std::string* model = config.FindString("model");
  if (model && !model->empty()) {
    model_id_ = *model;
  }

  // A non-empty base_url is a user override; an empty one means "use the
  // default" and drops a previous override. Absent leaves it unchanged.
  if (const std::string* base_url = config.FindString("base_url")) {
    base_url_overridden_ = !base_url->empty();
    base_url_ = base_url_overridden_ ? *base_url : default_base_url_;
  }

  return true;
}

std::string GoogleProvider::GetDefaultBaseUrl() const {
  return default_base_url_;
}

void GoogleProvider::SetDefaultBaseUrl(const std::string& url) {
  default_base_url_ = url;
  if (!base_url_overridden_) {
    base_url_ = default_base_url_;
  }
}

std::string GoogleProvider::GetBaseUrl() const {
  return base_url_;
}

void GoogleProvider::FetchAvailableModels(FetchModelsCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (api_key_.empty()) {
    std::move(callback).Run({});
    return;
  }

  // Coalesce concurrent fetches — replacing models_loader_ mid-flight would
  // silently drop the earlier caller's callback (SimpleURLLoader never runs
  // it on destruction), hanging NexusLlmHost::Catalog()'s barrier.
  if (models_loader_) {
    pending_models_callbacks_.push_back(std::move(callback));
    return;
  }
  callback = base::BindOnce(
      [](base::WeakPtr<GoogleProvider> self, FetchModelsCallback primary,
         std::vector<ModelInfo> models) {
        std::move(primary).Run(models);
        if (self) {
          std::vector<FetchModelsCallback> waiting =
              std::move(self->pending_models_callbacks_);
          self->pending_models_callbacks_.clear();
          for (FetchModelsCallback& cb : waiting) {
            std::move(cb).Run(models);
          }
        }
      },
      weak_factory_.GetWeakPtr(), std::move(callback));

  std::string api_base = base_url_;
  if (!api_base.empty() && api_base.back() == '/') {
    api_base.pop_back();
  }

  GURL url(base::StrCat({api_base, "/models?key=", api_key_}));

  auto resource_request = std::make_unique<network::ResourceRequest>();
  resource_request->url = url;
  resource_request->method = "GET";
  ApplyUserAgent(resource_request->headers);
  resource_request->credentials_mode =
      network::mojom::CredentialsMode::kOmit;

  models_loader_ =
      network::SimpleURLLoader::Create(std::move(resource_request),
                                       kTrafficAnnotation);

  models_loader_->DownloadToString(
      url_loader_factory_.get(),
      base::BindOnce(&GoogleProvider::OnFetchModelsComplete,
                     weak_factory_.GetWeakPtr(), std::move(callback)),
      kMaxResponseSizeBytes);
}

void GoogleProvider::OnFetchModelsComplete(
    FetchModelsCallback callback,
    std::optional<std::string> response_body) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  models_loader_.reset();

  // Capability flags and filtering (e.g. excluding embedding models) all
  // live in the TS layer. The native provider passes through whatever the
  // Google /models endpoint returned. We still strip the "models/" prefix
  // because that is wire-format normalisation, not model identity.
  if (!response_body) {
    std::move(callback).Run({});
    return;
  }

  // Cache the raw JSON as soon as we have a network-successful body. Any
  // downstream parse failure should still leave the prefs cache populated so
  // the TS layer has something to render.
  last_raw_catalog_json_ = *response_body;

  auto parsed =
      base::JSONReader::ReadDict(*response_body, base::JSON_PARSE_RFC);
  if (!parsed) {
    std::move(callback).Run({});
    return;
  }

  const auto* models_list = parsed->FindList("models");
  if (!models_list) {
    std::move(callback).Run({});
    return;
  }

  std::vector<ModelInfo> models;
  for (const auto& model_val : *models_list) {
    const base::DictValue* model = model_val.GetIfDict();
    if (!model)
      continue;

    const std::string* name_str = model->FindString("name");
    if (!name_str || name_str->empty())
      continue;

    std::string id = *name_str;
    if (base::StartsWith(id, "models/"))
      id = id.substr(7);

    const std::string* display_name = model->FindString("displayName");
    std::string model_name = display_name && !display_name->empty()
                                ? *display_name
                                : id;

    ModelInfo info;
    info.id = std::move(id);
    info.name = std::move(model_name);
    models.push_back(std::move(info));
  }

  fetched_models_ = models;
  std::move(callback).Run(std::move(models));
}

void GoogleProvider::LoadCachedCatalogJson(const std::string& raw_json) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (raw_json.empty())
    return;
  OnFetchModelsComplete(base::DoNothing(), raw_json);
}

std::string GoogleProvider::GetLastFetchedRawCatalogJson() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return last_raw_catalog_json_;
}

}  // namespace nexus
