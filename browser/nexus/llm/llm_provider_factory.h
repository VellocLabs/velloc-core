#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_LLM_LLM_PROVIDER_FACTORY_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_LLM_LLM_PROVIDER_FACTORY_H_

#include <memory>
#include <string>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "custom_browser/browser/nexus/llm/llm_types.h"

namespace network {
class SharedURLLoaderFactory;
}  // namespace network

namespace nexus {

class LlmProvider;

// Creates LlmProvider instances by provider ID.
//
// Adding a new provider:
//   1. Create files in providers/{name}/.
//   2. Register the ID in CreateProvider() and GetRegisteredProviderIds().
class LlmProviderFactory {
 public:
  LlmProviderFactory() = delete;

  // Creates and returns a provider instance for |provider_id|.
  // Returns nullptr if the ID is unknown.
  static std::unique_ptr<LlmProvider> CreateProvider(
      const std::string& provider_id,
      scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory);

  // Returns the IDs of every registered provider, in display order.
  static std::vector<std::string> GetRegisteredProviderIds();

  // Returns metadata for every registered provider. This creates temporary
  // provider instances to collect their ModelInfo, display names, etc.
  static std::vector<ProviderInfo> GetRegisteredProviders(
      scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory);
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_LLM_LLM_PROVIDER_FACTORY_H_
