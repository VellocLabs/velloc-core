#include "custom_browser/browser/nexus/llm/llm_provider.h"

#include <utility>

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

}  // namespace nexus
