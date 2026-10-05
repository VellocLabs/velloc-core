#include "custom_browser/browser/nexus/api/llm/llm_provider.h"

#include <utility>

#include "net/http/http_request_headers.h"

namespace nexus {

void LlmProvider::FetchAvailableModels(FetchModelsCallback callback) {
  std::move(callback).Run(GetSupportedModels());
}

void LlmProvider::LoadCachedCatalogJson(const std::string& raw_json) {
  // Providers that support catalogue caching override this. The default
  // no-op is fine for providers whose catalogue is inherently volatile
  // (e.g. Ollama — depends on the user's local server at request time).
}

std::string LlmProvider::GetLastFetchedRawCatalogJson() const {
  return std::string();
}

void LlmProvider::ConfigureUserAgent(const base::DictValue& config) {
  const std::string* user_agent = config.FindString("user_agent");
  user_agent_ = user_agent ? *user_agent : std::string();
}

void LlmProvider::ApplyUserAgent(net::HttpRequestHeaders& headers) const {
  if (!user_agent_.empty()) {
    headers.SetHeader(net::HttpRequestHeaders::kUserAgent, user_agent_);
  }
}

}  // namespace nexus
