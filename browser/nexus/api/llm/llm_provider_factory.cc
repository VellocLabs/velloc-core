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
// convention, so only identity and default base URL differ.
struct OpenAiCompatConfig {
  const char* id;
  const char* display_name;
  const char* default_base_url;
};

constexpr OpenAiCompatConfig kOpenAiCompatConfigs[] = {
    {"deepseek", "DeepSeek", "https://api.deepseek.com"},
    {"glm", "GLM", "https://open.bigmodel.cn/api/paas/v4"},
    {"kimi", "Kimi", "https://api.moonshot.cn/v1"},
    {"minimax", "MiniMax", "https://api.minimax.io/v1"},
    {"qwen", "Qwen", "https://dashscope.aliyuncs.com/compatible-mode/v1"},
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
  // it in Settings → Models) and no required API key — many compatible
  // endpoints run keyless (e.g. cursor-api-proxy, LM Studio, local gateways).
  // The wire dialect is chosen by which class we instantiate: custom-openai
  // speaks OpenAI /chat/completions, custom-anthropic speaks the Anthropic
  // /v1/messages API.
  if (provider_id == "custom-openai") {
    return std::make_unique<OpenAiProvider>(
        url_loader_factory, "custom-openai", "Custom (OpenAI-compatible)",
        /*default_base_url=*/std::string(), /*uses_responses_api=*/false,
        /*extra_headers=*/std::map<std::string, std::string>{},
        /*requires_api_key=*/false);
  }
  if (provider_id == "custom-anthropic") {
    return std::make_unique<AnthropicProvider>(
        url_loader_factory, "custom-anthropic", "Custom (Anthropic-compatible)",
        /*default_base_url=*/std::string(), /*requires_api_key=*/false);
  }

  for (const auto& config : kOpenAiCompatConfigs) {
    if (provider_id == config.id) {
      return std::make_unique<OpenAiProvider>(
          url_loader_factory, config.id, config.display_name,
          config.default_base_url);
    }
  }

  DLOG(ERROR) << "Unknown LLM provider ID: " << provider_id;
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
