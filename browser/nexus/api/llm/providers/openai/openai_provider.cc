#include "custom_browser/browser/nexus/api/llm/providers/openai/openai_provider.h"

#include "custom_browser/browser/nexus/nexus_secret_crypt.h"

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
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "net/http/http_response_headers.h"
#include "url/gurl.h"
#include "url/gurl.h"

namespace nexus {

namespace {

constexpr std::string_view kOpenAiProviderId = "openai";
constexpr std::string_view kOpenAiDisplayName = "OpenAI";

constexpr int kMaxResponseSizeBytes = 4 * 1024 * 1024;

// Sent as `?client_version=` when listing models from the Codex backend. The
// backend hides any model whose `minimal_client_version` exceeds this, so it
// must stay >= the newest model's minimum (e.g. gpt-5.5 needs 0.124.0). We
// claim a high version to receive the account's full model list; bump if a
// future model sets an even higher minimum.
constexpr char kCodexClientVersion[] = "0.500.0";

// Curated model list for Qwen Code (qwen-code OAuth). The Qwen portal does not
// expose an OpenAI-style /models endpoint, so we fall back to the free-tier
// coder models the qwen-code CLI uses when discovery returns nothing.
std::vector<ModelInfo> CuratedQwenCodeModels() {
  std::vector<ModelInfo> models;
  {
    ModelInfo plus;
    plus.id = "qwen3-coder-plus";
    plus.name = "Qwen3 Coder Plus";
    plus.context_window = 1048576;
    plus.supports_tools = true;
    models.push_back(std::move(plus));
  }
  {
    ModelInfo flash;
    flash.id = "qwen3-coder-flash";
    flash.name = "Qwen3 Coder Flash";
    flash.context_window = 1048576;
    flash.supports_tools = true;
    models.push_back(std::move(flash));
  }
  return models;
}

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("nexus_openai_llm", R"(
      semantics {
        sender: "Nexus AI Agent Platform"
        description:
          "Sends chat completion requests to the OpenAI API on behalf of "
          "the user's configured AI agent. Requests contain conversation "
          "messages and optional tool definitions."
        trigger:
          "User interacts with the Nexus AI agent panel, triggering an "
          "LLM inference request through the configured OpenAI provider."
        data:
          "Conversation messages, model parameters, and optional tool "
          "schemas. May include user-authored text and page content the "
          "user has explicitly shared with the agent."
        destination: OTHER
        internal {
          contacts { email: "nicke@nicke.dev" }
        }
        user_data {
          type: OTHER
        }
        last_reviewed: "2026-03-13"
      }
      policy {
        cookies_allowed: NO
        setting:
          "Users configure their own OpenAI API key in Shift Browser "
          "settings under the Nexus AI section. No requests are sent "
          "without an API key."
        policy_exception_justification:
          "User-initiated AI feature with explicit opt-in and API key "
          "configuration."
      }
    )");

}  // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

OpenAiProvider::OpenAiProvider(
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory)
    : OpenAiProvider(std::move(url_loader_factory),
                     std::string(kOpenAiProviderId),
                     std::string(kOpenAiDisplayName),
                     /*default_base_url=*/std::string()) {}

OpenAiProvider::OpenAiProvider(
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
    std::string provider_id,
    std::string display_name,
    std::string default_base_url)
    : OpenAiProvider(std::move(url_loader_factory),
                     provider_id,
                     std::move(display_name),
                     std::move(default_base_url),
                     /*extra_headers=*/{}) {}

OpenAiProvider::OpenAiProvider(
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
    std::string provider_id,
    std::string display_name,
    std::string default_base_url,
    std::map<std::string, std::string> extra_headers,
    bool requires_api_key)
    : provider_id_(std::move(provider_id)),
      display_name_(std::move(display_name)),
      default_base_url_(std::move(default_base_url)),
      base_url_(default_base_url_),
      requires_api_key_(requires_api_key),
      extra_headers_(std::move(extra_headers)),
      url_loader_factory_(std::move(url_loader_factory)) {}

OpenAiProvider::~OpenAiProvider() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

// ---------------------------------------------------------------------------
// LlmProvider property accessors
// ---------------------------------------------------------------------------

std::string OpenAiProvider::GetId() const {
  return provider_id_;
}

std::string OpenAiProvider::GetDisplayName() const {
  return display_name_;
}

LlmProviderType OpenAiProvider::GetProviderType() const {
  return LlmProviderType::kCloud;
}

std::vector<ModelInfo> OpenAiProvider::GetSupportedModels() const {
  return fetched_models_;
}

std::string OpenAiProvider::GetDefaultModelId() const {
  // Default-model selection is owned by the TS layer (nexus_web).
  return std::string();
}

bool OpenAiProvider::SupportsStreaming() const {
  return true;
}

bool OpenAiProvider::SupportsTools() const {
  return true;
}

bool OpenAiProvider::IsConfigured() const {
  // Custom/proxy providers may run keyless (requires_api_key_ == false); they
  // count as configured once a base URL is set.
  return requires_api_key_ ? !api_key_.empty() : !base_url_.empty();
}

bool OpenAiProvider::Configure(const base::DictValue& config) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  ConfigureUserAgent(config);

  const std::string* api_key = config.FindString("api_key");
  if (api_key && !api_key->empty()) {
    api_key_ = NexusSecretCrypt::GetInstance()->Decrypt(*api_key);
  } else if (requires_api_key_) {
    DLOG(ERROR) << display_name_ << " provider requires a non-empty api_key";
    return false;
  }

  // A non-empty base_url is a user override; an empty one means "use the
  // default" and drops a previous override. Absent leaves it unchanged.
  if (const std::string* base_url = config.FindString("base_url")) {
    base_url_overridden_ = !base_url->empty();
    base_url_ = base_url_overridden_ ? *base_url : default_base_url_;
  }

  if (const std::string* model = config.FindString("model");
      model && !model->empty()) {
    configured_model_ = *model;
  }

  // OAuth-backed providers (chatgpt-pro) pass the ChatGPT account id so it can
  // be sent as the `ChatGPT-Account-Id` header. Absent for other providers.
  if (const std::string* account_id = config.FindString("account_id");
      account_id) {
    account_id_ = *account_id;
  }

  return IsConfigured();
}

void OpenAiProvider::ApplyExtraHeaders(
    network::ResourceRequest* request) const {
  for (const auto& [name, value] : extra_headers_) {
    request->headers.SetHeader(name, value);
  }
  if (!account_id_.empty()) {
    request->headers.SetHeader("ChatGPT-Account-Id", account_id_);
  }
  // Last, so a pinned preset User-Agent wins over a compiled-in extra header.
  ApplyUserAgent(request->headers);
}

std::string OpenAiProvider::GetDefaultBaseUrl() const {
  return default_base_url_;
}

void OpenAiProvider::SetDefaultBaseUrl(const std::string& url) {
  default_base_url_ = url;
  if (!base_url_overridden_) {
    base_url_ = default_base_url_;
  }
}

std::string OpenAiProvider::GetBaseUrl() const {
  return base_url_;
}

// ---------------------------------------------------------------------------
// ChatGPT (Codex) backend detection
// ---------------------------------------------------------------------------

bool OpenAiProvider::IsCodexBackend() const {
  return base_url_.find("/backend-api/codex") != std::string::npos;
}

// ---------------------------------------------------------------------------
// FetchAvailableModels — GET /v1/models
// ---------------------------------------------------------------------------

void OpenAiProvider::FetchAvailableModels(FetchModelsCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (!IsConfigured()) {
    std::move(callback).Run({});
    return;
  }

  // Coalesce concurrent fetches. The previous pattern replaced
  // models_loader_, which silently DROPPED the in-flight caller's callback
  // (SimpleURLLoader never runs it on destruction) — upstream,
  // NexusLlmHost::Catalog()'s BarrierClosure then never fired and the
  // guest's catalog call hung forever.
  if (models_loader_) {
    pending_models_callbacks_.push_back(std::move(callback));
    return;
  }
  callback = base::BindOnce(
      [](base::WeakPtr<OpenAiProvider> self, FetchModelsCallback primary,
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

  const bool codex = IsCodexBackend();
  std::string models_url = base_url_ + "/models";
  if (codex) {
    // The Codex backend gates the model list on a client-version query param
    // and returns exactly the models the signed-in ChatGPT account can use
    // (mirrors the Codex CLI's `GET /models?client_version=…`).
    models_url += "?client_version=";
    models_url += kCodexClientVersion;
  }

  auto request = std::make_unique<network::ResourceRequest>();
  request->url = GURL(models_url);
  request->method = "GET";
  request->headers.SetHeader("Authorization", "Bearer " + api_key_);
  // Apply per-provider headers (e.g. Codex originator/account-id, Copilot
  // Openai-Intent/x-initiator) so /models discovery is accepted. No-op for
  // providers without extra headers.
  ApplyExtraHeaders(request.get());

  models_loader_ =
      network::SimpleURLLoader::Create(std::move(request), kTrafficAnnotation);

  models_loader_->DownloadToString(
      url_loader_factory_.get(),
      base::BindOnce(&OpenAiProvider::OnFetchModelsComplete,
                     weak_factory_.GetWeakPtr(), std::move(callback)),
      kMaxResponseSizeBytes);
}

void OpenAiProvider::OnFetchModelsComplete(
    FetchModelsCallback callback,
    std::optional<std::string> response_body) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  models_loader_.reset();

  // Capability flags, context-window size and filtering/display overrides
  // all live in the TS layer. The native provider passes through whatever
  // the /v1/models endpoint returned. OpenAI's /v1/models lists every kind
  // of model (embeddings, audio, images, chat); TS is responsible for
  // filtering to chat-capable entries via the curated model config.
  if (!response_body) {
    DLOG(WARNING) << display_name_ << " models fetch failed";
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
    DLOG(WARNING) << display_name_ << " models: invalid JSON";
    std::move(callback).Run({});
    return;
  }

  std::vector<ModelInfo> models;

  if (IsCodexBackend()) {
    // Codex backend shape: {"models":[{"slug","display_name",
    // "default_reasoning_level","supported_reasoning_levels":[{"effort"}]}]}.
    const base::ListValue* codex_models = parsed->FindList("models");
    if (!codex_models) {
      DLOG(WARNING) << display_name_ << " codex models: missing 'models'";
      std::move(callback).Run({});
      return;
    }
    for (const auto& model_val : *codex_models) {
      const base::DictValue* model = model_val.GetIfDict();
      if (!model)
        continue;
      const std::string* slug = model->FindString("slug");
      if (!slug || slug->empty())
        continue;

      // Only models marked for listing belong in the picker; "hide"/"none"
      // are internal/deprecated. Absent visibility is treated as listable.
      if (const std::string* visibility = model->FindString("visibility");
          visibility && *visibility != "list") {
        continue;
      }

      ModelInfo info;
      info.id = *slug;
      if (const std::string* display = model->FindString("display_name");
          display && !display->empty()) {
        info.name = *display;
      } else {
        info.name = *slug;
      }
      info.supports_tools = true;
      if (const std::string* level =
              model->FindString("default_reasoning_level");
          level && !level->empty()) {
        info.default_thinking_tier = *level;
      }
      if (const base::ListValue* levels =
              model->FindList("supported_reasoning_levels")) {
        for (const auto& level_val : *levels) {
          if (const base::DictValue* level_dict = level_val.GetIfDict()) {
            if (const std::string* effort = level_dict->FindString("effort");
                effort && !effort->empty()) {
              info.available_thinking_tiers.push_back(*effort);
            }
          }
        }
      }
      models.push_back(std::move(info));
    }

    fetched_models_ = models;
    std::move(callback).Run(std::move(models));
    return;
  }

  if (const base::ListValue* data = parsed->FindList("data")) {
    for (const auto& model_val : *data) {
      const base::DictValue* model = model_val.GetIfDict();
      if (!model)
        continue;

      const std::string* id = model->FindString("id");
      if (!id || id->empty())
        continue;

      ModelInfo info;
      info.id = *id;
      info.name = *id;

      models.push_back(std::move(info));
    }
  }

  // Qwen Code's portal does not expose an OpenAI-style /models list, so
  // discovery typically comes back empty. Fall back to the curated free-tier
  // coder models the qwen-code CLI ships with, so the picker still populates.
  if (models.empty() && provider_id_ == "qwen-code") {
    models = CuratedQwenCodeModels();
  }

  fetched_models_ = models;
  std::move(callback).Run(std::move(models));
}

void OpenAiProvider::LoadCachedCatalogJson(const std::string& raw_json) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (raw_json.empty())
    return;
  OnFetchModelsComplete(base::DoNothing(), raw_json);
}

std::string OpenAiProvider::GetLastFetchedRawCatalogJson() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return last_raw_catalog_json_;
}

}  // namespace nexus
