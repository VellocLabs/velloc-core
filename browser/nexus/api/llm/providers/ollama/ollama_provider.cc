#include "custom_browser/browser/nexus/api/llm/providers/ollama/ollama_provider.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "url/gurl.h"

namespace nexus {

namespace {

constexpr int kModelFetchTimeoutSeconds = 10;

// Upper bound for non-streaming completion bodies and the /api/tags model
// catalogue. Replaces DownloadToStringOfUnboundedSizeUntilCrashAndDie — a
// misbehaving (or merely mis-addressed) server must not be able to OOM the
// browser process.
constexpr size_t kMaxResponseSizeBytes =
    network::SimpleURLLoader::kMaxBoundedStringDownloadSize;

const net::NetworkTrafficAnnotationTag kOllamaTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("nexus_ollama_provider", R"(
        semantics {
          sender: "Nexus AI Agent Platform"
          description:
            "Sends chat completion requests to a locally running Ollama "
            "server for AI agent functionality."
          trigger:
            "User initiates an AI agent action that requires LLM inference."
          data:
            "Chat messages, model configuration, and tool definitions."
          destination: LOCAL
        }
        policy {
          cookies_allowed: NO
        setting:
            "Users can configure the Ollama server address in Nexus "
            "settings."
        })");

}  // namespace

// --- OllamaProvider ---

OllamaProvider::OllamaProvider(
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory)
    : url_loader_factory_(std::move(url_loader_factory)) {
  DETACH_FROM_SEQUENCE(sequence_checker_);
}

OllamaProvider::~OllamaProvider() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

std::string OllamaProvider::GetId() const {
  return "ollama";
}

std::string OllamaProvider::GetDisplayName() const {
  return "Ollama (Local)";
}

LlmProviderType OllamaProvider::GetProviderType() const {
  return LlmProviderType::kLocal;
}

std::vector<ModelInfo> OllamaProvider::GetSupportedModels() const {
  return fetched_models_;
}

std::string OllamaProvider::GetDefaultModelId() const {
  // Default-model selection is owned by the TS layer (nexus_web).
  return std::string();
}

bool OllamaProvider::SupportsStreaming() const {
  return true;
}

bool OllamaProvider::SupportsTools() const {
  return true;
}

bool OllamaProvider::IsConfigured() const {
  // Keyless: usable once it has somewhere to send requests — the guest's
  // catalog default or a user override.
  return !base_url_.empty();
}

bool OllamaProvider::Configure(const base::DictValue& config) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  ConfigureUserAgent(config);

  // A non-empty base_url is a user override; an empty one means "use the
  // default" and drops a previous override. Absent leaves it unchanged.
  if (const auto* base_url = config.FindString("base_url")) {
    base_url_overridden_ = !base_url->empty();
    base_url_ = base_url_overridden_ ? *base_url : default_base_url_;
  }

  const auto* model = config.FindString("model");
  if (model && !model->empty()) {
    configured_model_ = *model;
  }

  return true;
}

std::string OllamaProvider::GetDefaultBaseUrl() const {
  return default_base_url_;
}

void OllamaProvider::SetDefaultBaseUrl(const std::string& url) {
  default_base_url_ = url;
  if (!base_url_overridden_) {
    base_url_ = default_base_url_;
  }
}

std::string OllamaProvider::GetBaseUrl() const {
  return base_url_;
}

void OllamaProvider::FetchAvailableModels(FetchModelsCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // Coalesce concurrent fetches — replacing models_loader_ mid-flight would
  // silently drop the earlier caller's callback (SimpleURLLoader never runs
  // it on destruction), hanging NexusLlmHost::Catalog()'s barrier.
  if (models_loader_) {
    pending_models_callbacks_.push_back(std::move(callback));
    return;
  }
  callback = base::BindOnce(
      [](base::WeakPtr<OllamaProvider> self, FetchModelsCallback primary,
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

  auto resource_request = std::make_unique<network::ResourceRequest>();
  resource_request->url = GURL(base::StrCat({base_url_, "/api/tags"}));
  resource_request->method = "GET";
  ApplyUserAgent(resource_request->headers);

  models_loader_ = network::SimpleURLLoader::Create(
      std::move(resource_request), kOllamaTrafficAnnotation);
  models_loader_->SetTimeoutDuration(
      base::Seconds(kModelFetchTimeoutSeconds));

  models_loader_->DownloadToString(
      url_loader_factory_.get(),
      base::BindOnce(&OllamaProvider::OnFetchModelsComplete,
                     weak_factory_.GetWeakPtr(), std::move(callback)),
      kMaxResponseSizeBytes);
}

// --- Dynamic model listing ---

void OllamaProvider::OnFetchModelsComplete(
    FetchModelsCallback callback,
    std::optional<std::string> response_body) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  models_loader_.reset();

  // Capability flags and context-window sizing live in the TS layer. The
  // Ollama catalogue is whatever `/api/tags` returns on the user's local
  // server; if the call fails there is nothing to fall back to.
  if (!response_body) {
    DLOG(ERROR) << "Failed to fetch Ollama models from " << base_url_;
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
    DLOG(ERROR) << "Failed to parse Ollama /api/tags response";
    std::move(callback).Run({});
    return;
  }

  const auto* models_list = parsed->FindList("models");
  if (!models_list || models_list->empty()) {
    std::move(callback).Run({});
    return;
  }

  std::vector<ModelInfo> models;
  for (const auto& model_val : *models_list) {
    if (!model_val.is_dict()) {
      continue;
    }
    const auto& model = model_val.GetDict();
    const auto* name = model.FindString("name");
    if (!name || name->empty()) {
      continue;
    }

    ModelInfo info;
    info.id = *name;
    auto colon_pos = name->find(':');
    info.name =
        (colon_pos != std::string::npos) ? name->substr(0, colon_pos) : *name;
    models.push_back(std::move(info));
  }

  fetched_models_ = models;
  std::move(callback).Run(std::move(models));
}

void OllamaProvider::LoadCachedCatalogJson(const std::string& raw_json) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (raw_json.empty())
    return;
  OnFetchModelsComplete(base::DoNothing(), raw_json);
}

std::string OllamaProvider::GetLastFetchedRawCatalogJson() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return last_raw_catalog_json_;
}

}  // namespace nexus
