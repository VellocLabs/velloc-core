#include "custom_browser/browser/nexus/api/llm/llm_provider_factory.h"

#include <map>
#include <utility>

#include "base/logging.h"
#include "custom_browser/browser/nexus/api/llm/llm_provider.h"
#include "custom_browser/browser/nexus/api/llm/oauth/oauth_providers.h"
#include "custom_browser/browser/nexus/api/llm/providers/anthropic/anthropic_provider.h"
#include "custom_browser/browser/nexus/api/llm/providers/google/google_provider.h"
#include "custom_browser/browser/nexus/api/llm/providers/ollama/ollama_provider.h"
#include "custom_browser/browser/nexus/api/llm/providers/openai/openai_provider.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"

namespace nexus {

namespace {

// Declaration order is the display order used by the Settings UI provider
// popup. See `LlmBroker::GetRegisteredProviders()` — it iterates this array
// so alphabetical map ordering doesn't leak through.
constexpr const char* kProviderIds[] = {
    "openai",
    "chatgpt-pro",
    "anthropic",
    "github-copilot",
    "qwen-code",
    "google",
    "glm",
    "minimax",
    "qwen",
    "kimi",
    "deepseek",
    "ollama",
    "custom-openai",
    "custom-anthropic",
};

// OpenAI-compatible providers. The request/response format, SSE streaming,
// /models discovery, and tool-calling schema all follow the OpenAI
// convention, so only identity differs here. Their default base URLs are
// NOT compiled in: the guest owns them (`defaultBaseUrl` in
// nexus_web/src/config/nexusModelConfig/providers/<id>.ts) and the broker
// applies them from the published catalog.
struct OpenAiCompatConfig {
  const char* id;
  const char* display_name;
};

constexpr OpenAiCompatConfig kOpenAiCompatConfigs[] = {
    {"deepseek", "DeepSeek"}, {"glm", "GLM"},   {"kimi", "Kimi"},
    {"minimax", "MiniMax"},   {"qwen", "Qwen"},
};

}  // namespace

// static
std::unique_ptr<LlmProvider> LlmProviderFactory::CreateProvider(
    const std::string& provider_id,
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory) {
  if (provider_id == "openai")
    return std::make_unique<OpenAiProvider>(url_loader_factory);

  // ChatGPT Plus/Pro (OAuth subscription). Reuses OpenAiProvider's Responses
  // API path against the ChatGPT Codex backend, with the originator /
  // OpenAI-Beta headers that backend expects. The OAuth access token is
  // supplied as the api_key via Configure() (see NexusOAuthManager).
  if (provider_id == "chatgpt-pro") {
    const OAuthProviderConfig* config = GetOAuthProviderConfig(provider_id);
    if (!config) {
      DLOG(ERROR) << "Missing OAuth config for chatgpt-pro";
      return nullptr;
    }
    std::map<std::string, std::string> extra_headers = {
        {"originator", "browseros"},
        {"OpenAI-Beta", "responses=experimental"},
    };
    // Concurrency / idle-timeout for this provider is driven by the TS
    // model config (nexusModelConfig/providers/chatgptPro.ts
    // `maxConcurrentStreams: 1`), pushed here via publishProviderConfigs ->
    // Configure(), NOT hard-coded — a settings.json override still wins.
    return std::make_unique<OpenAiProvider>(
        url_loader_factory, config->id, config->display_name,
        config->backend_base_url, /*uses_responses_api=*/true,
        std::move(extra_headers));
  }

  // GitHub Copilot + Qwen Code (OAuth device-code). Both speak OpenAI-style
  // chat/completions against their own base URL; the OAuth access token is
  // supplied as the api_key via Configure(). Copilot needs a couple of extra
  // request headers (mirrors BrowserOS's copilot-fetch).
  if (provider_id == "github-copilot" || provider_id == "qwen-code") {
    const OAuthProviderConfig* config = GetOAuthProviderConfig(provider_id);
    if (!config) {
      DLOG(ERROR) << "Missing OAuth config for " << provider_id;
      return nullptr;
    }
    std::map<std::string, std::string> extra_headers;
    if (provider_id == "github-copilot") {
      // api.githubcopilot.com rejects requests without an integration id and
      // editor version (403). These mirror the GitHub Copilot Chat client so
      // both /models discovery and chat/completions are accepted. The bearer
      // is the short-lived Copilot token minted by NexusOAuthManager from the
      // GitHub OAuth token (see EnsureCopilotToken).
      extra_headers = {
          {"Copilot-Integration-Id", "vscode-chat"},
          {"Editor-Version", "vscode/1.96.0"},
          {"Editor-Plugin-Version", "copilot-chat/0.23.0"},
          {"Openai-Intent", "conversation-edits"},
          {"x-initiator", "user"},
      };
    }
    return std::make_unique<OpenAiProvider>(
        url_loader_factory, config->id, config->display_name,
        config->backend_base_url, /*uses_responses_api=*/false,
        std::move(extra_headers));
  }

  if (provider_id == "anthropic")
    return std::make_unique<AnthropicProvider>(url_loader_factory);

  if (provider_id == "google")
    return std::make_unique<GoogleProvider>(url_loader_factory);

  if (provider_id == "ollama")
    return std::make_unique<OllamaProvider>(url_loader_factory);

  // User-configurable custom endpoints. No default base URL (the user supplies
  // it in Settings → Provider connection) and no required API key — many
  // compatible endpoints run keyless (e.g. cursor-api-proxy, LM Studio, local
  // gateways). The wire dialect is chosen by which class we instantiate:
  // custom-openai speaks OpenAI /chat/completions, custom-anthropic speaks the
  // Anthropic /v1/messages API.
  //
  // These two are the only custom endpoints with a compiled-in id. A provider
  // the user CREATES gets its own instance through CreateCustomProvider(),
  // built the same way with the user's id and name.
  if (provider_id == "custom-openai") {
    return CreateCustomProvider(provider_id, "Custom (OpenAI-compatible)",
                                "openai", url_loader_factory);
  }
  if (provider_id == "custom-anthropic") {
    return CreateCustomProvider(provider_id, "Custom (Anthropic-compatible)",
                                "anthropic", url_loader_factory);
  }

  for (const auto& config : kOpenAiCompatConfigs) {
    if (provider_id == config.id) {
      return std::make_unique<OpenAiProvider>(
          url_loader_factory, config.id, config.display_name,
          /*default_base_url=*/std::string());
    }
  }

  DLOG(ERROR) << "Unknown LLM provider ID: " << provider_id;
  return nullptr;
}

// static
std::unique_ptr<LlmProvider> LlmProviderFactory::CreateCustomProvider(
    const std::string& provider_id,
    const std::string& display_name,
    const std::string& api_compatibility,
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory) {
  if (provider_id.empty()) {
    return nullptr;
  }
  const std::string& name = display_name.empty() ? provider_id : display_name;

  if (api_compatibility == "openai") {
    return std::make_unique<OpenAiProvider>(
        url_loader_factory, provider_id, name,
        /*default_base_url=*/std::string(), /*uses_responses_api=*/false,
        /*extra_headers=*/std::map<std::string, std::string>{},
        /*requires_api_key=*/false);
  }
  if (api_compatibility == "anthropic") {
    return std::make_unique<AnthropicProvider>(
        url_loader_factory, provider_id, name,
        /*default_base_url=*/std::string(), /*requires_api_key=*/false);
  }

  DLOG(ERROR) << "Unknown API compatibility '" << api_compatibility
              << "' for custom provider " << provider_id;
  return nullptr;
}

// static
std::vector<std::string> LlmProviderFactory::GetRegisteredProviderIds() {
  return std::vector<std::string>(std::begin(kProviderIds),
                                  std::end(kProviderIds));
}

// static
std::vector<ProviderInfo> LlmProviderFactory::GetRegisteredProviders(
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory) {
  std::vector<ProviderInfo> infos;
  infos.reserve(std::size(kProviderIds));

  for (const char* id : kProviderIds) {
    auto provider = CreateProvider(id, url_loader_factory);
    if (!provider)
      continue;

    ProviderInfo info;
    info.id = provider->GetId();
    info.name = provider->GetDisplayName();
    info.type = provider->GetProviderType();
    info.is_configured = provider->IsConfigured();
    info.models = provider->GetSupportedModels();
    infos.push_back(std::move(info));
  }

  return infos;
}

}  // namespace nexus
