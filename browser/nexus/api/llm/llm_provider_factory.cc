#include "custom_browser/browser/nexus/api/llm/llm_provider_factory.h"

#include <utility>

#include "base/logging.h"
#include "custom_browser/browser/nexus/api/llm/llm_provider.h"
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
    "anthropic",
    "google",
    "glm",
    "minimax",
    "qwen",
    "kimi",
    "ollama",
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

  if (provider_id == "anthropic")
    return std::make_unique<AnthropicProvider>(url_loader_factory);

  if (provider_id == "google")
    return std::make_unique<GoogleProvider>(url_loader_factory);

  if (provider_id == "ollama")
    return std::make_unique<OllamaProvider>(url_loader_factory);

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
