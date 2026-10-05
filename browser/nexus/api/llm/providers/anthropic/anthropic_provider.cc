#include "custom_browser/browser/nexus/api/llm/providers/anthropic/anthropic_provider.h"

#include "custom_browser/browser/nexus/nexus_secret_crypt.h"

#include <array>
#include <utility>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "net/base/net_errors.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/resource_request_body.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "url/gurl.h"

namespace nexus {

namespace {

constexpr std::string_view kAnthropicProviderId = "anthropic";
constexpr std::string_view kAnthropicDisplayName = "Anthropic";
constexpr size_t kMaxResponseSizeBytes =
    network::SimpleURLLoader::kMaxBoundedStringDownloadSize;

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("nexus_anthropic_provider", R"(
      semantics {
        sender: "Velloc AI Agent Platform"
        description:
          "Sends prompts to the Anthropic Messages API for AI text "
          "generation as part of the Velloc Code platform."
        trigger:
          "User interacts with the Velloc AI agent panel, which "
          "triggers LLM inference through the configured Anthropic "
          "provider."
        data:
          "User-provided prompts, conversation history, and tool "
          "definitions. No personal data is sent beyond what the user "
          "explicitly provides in the conversation."
        destination: OTHER
        internal {
          contacts {
            email: "niclas@niclas.cc"
          }
        }
        user_data {
          type: OTHER
        }
        last_reviewed: "2026-03-13"
      }
      policy {
        cookies_allowed: NO
        setting:
          "Users configure the Anthropic provider and API key in "
          "Shift Settings > AI Agent > Provider."
        policy_exception_justification:
          "Not yet covered by enterprise policy."
      }
    )");

}  // namespace

// ---------------------------------------------------------------------------
// AnthropicProvider
// ---------------------------------------------------------------------------

AnthropicProvider::AnthropicProvider(
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory)
    : AnthropicProvider(std::move(url_loader_factory),
                        std::string(kAnthropicProviderId),
                        std::string(kAnthropicDisplayName),
                        /*default_base_url=*/std::string()) {}

AnthropicProvider::AnthropicProvider(
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
    std::string provider_id,
    std::string display_name,
    std::string default_base_url,
    bool requires_api_key)
    : provider_id_(std::move(provider_id)),
      display_name_(std::move(display_name)),
      default_base_url_(std::move(default_base_url)),
      requires_api_key_(requires_api_key),
      base_url_(default_base_url_),
      url_loader_factory_(std::move(url_loader_factory)) {}

AnthropicProvider::~AnthropicProvider() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

std::string AnthropicProvider::GetId() const {
  return provider_id_;
}

std::string AnthropicProvider::GetDisplayName() const {
  return display_name_;
}

LlmProviderType AnthropicProvider::GetProviderType() const {
  return LlmProviderType::kCloud;
}

std::vector<ModelInfo> AnthropicProvider::GetSupportedModels() const {
  return fetched_models_;
}

std::string AnthropicProvider::GetDefaultModelId() const {
  // Default-model selection is owned by the TS layer (nexus_web); the native
  // provider no longer publishes a hard-coded default.
  return std::string();
}

bool AnthropicProvider::SupportsStreaming() const {
  return true;
}

bool AnthropicProvider::SupportsTools() const {
  return true;
}

bool AnthropicProvider::IsConfigured() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // Custom/proxy providers may run keyless (requires_api_key_ == false); they
  // count as configured once a base URL is set.
  return requires_api_key_ ? !api_key_.empty() : !base_url_.empty();
}

bool AnthropicProvider::Configure(const base::DictValue& config) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  ConfigureUserAgent(config);

  if (const auto* key = config.FindString("api_key"))
    api_key_ = NexusSecretCrypt::GetInstance()->Decrypt(*key);

  if (const auto* model = config.FindString("model");
      model && !model->empty()) {
    model_id_ = *model;
  }

  // A non-empty base_url is a user override; an empty one means "use the
  // default" and drops a previous override. Absent leaves it unchanged.
  if (const auto* base_url = config.FindString("base_url")) {
    base_url_overridden_ = !base_url->empty();
    base_url_ = base_url_overridden_ ? *base_url : default_base_url_;
  }

  return IsConfigured();
}

// ---------------------------------------------------------------------------
// FetchAvailableModels — GET /v1/models
// ---------------------------------------------------------------------------

void AnthropicProvider::FetchAvailableModels(FetchModelsCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (!IsConfigured()) {
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
      [](base::WeakPtr<AnthropicProvider> self, FetchModelsCallback primary,
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
      weak_ptr_factory_.GetWeakPtr(), std::move(callback));

  auto request = std::make_unique<network::ResourceRequest>();
  request->url = GURL(base_url_ + "/v1/models");
  request->method = "GET";
  request->headers.SetHeader("x-api-key", api_key_);
  request->headers.SetHeader("anthropic-version", "2023-06-01");
  ApplyUserAgent(request->headers);
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;

  models_loader_ =
      network::SimpleURLLoader::Create(std::move(request), kTrafficAnnotation);

  models_loader_->DownloadToString(
      url_loader_factory_.get(),
      base::BindOnce(&AnthropicProvider::OnFetchModelsComplete,
                     weak_ptr_factory_.GetWeakPtr(), std::move(callback)),
      kMaxResponseSizeBytes);
}

void AnthropicProvider::OnFetchModelsComplete(
    FetchModelsCallback callback,
    std::optional<std::string> response_body) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  models_loader_.reset();

  // Capability flags (supports_tools, supports_vision), context-window size
  // and filtering/display overrides all live in the TS layer. The native
  // provider passes through whatever the /v1/models endpoint returned.
  if (!response_body) {
    std::move(callback).Run({});
    return;
  }

  // Cache the raw JSON as soon as we have a network-successful body. Any
  // downstream parse failure should still leave the prefs cache populated so
  // the TS layer has something to render and a later code change can revisit
  // parsing without a round trip.
  last_raw_catalog_json_ = *response_body;

  auto parsed =
      base::JSONReader::ReadDict(*response_body, base::JSON_PARSE_RFC);
  if (!parsed) {
    std::move(callback).Run({});
    return;
  }

  const base::ListValue* data = parsed->FindList("data");
  if (!data) {
    std::move(callback).Run({});
    return;
  }

  std::vector<ModelInfo> models;
  for (const auto& model_val : *data) {
    const base::DictValue* model = model_val.GetIfDict();
    if (!model)
      continue;

    const std::string* id = model->FindString("id");
    if (!id || id->empty())
      continue;

    ModelInfo info;
    info.id = *id;
    const std::string* display_name = model->FindString("display_name");
    info.name = display_name ? *display_name : *id;

    models.push_back(std::move(info));
  }

  fetched_models_ = models;
  std::move(callback).Run(std::move(models));
}

void AnthropicProvider::LoadCachedCatalogJson(const std::string& raw_json) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (raw_json.empty())
    return;
  OnFetchModelsComplete(base::DoNothing(), raw_json);
}

std::string AnthropicProvider::GetLastFetchedRawCatalogJson() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return last_raw_catalog_json_;
}

// ---------------------------------------------------------------------------
// Network helpers
// ---------------------------------------------------------------------------

std::string AnthropicProvider::GetDefaultBaseUrl() const {
  return default_base_url_;
}

void AnthropicProvider::SetDefaultBaseUrl(const std::string& url) {
  default_base_url_ = url;
  if (!base_url_overridden_) {
    base_url_ = default_base_url_;
  }
}

std::string AnthropicProvider::GetBaseUrl() const {
  return base_url_;
}

}  // namespace nexus
