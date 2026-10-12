#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_PROVIDER_FACTORY_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_PROVIDER_FACTORY_H_

#include <memory>
#include <string>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "custom_browser/browser/nexus/api/llm/llm_types.h"

namespace network {
class SharedURLLoaderFactory;
}  // namespace network

namespace nexus {

class LlmProvider;

// Creates LlmProvider instances by provider ID. A new provider is registered
// in CreateProvider() and kProviderIds; an API-key vendor's default base URL
// is set on its nexus_web provider config, not here.
class LlmProviderFactory {
 public:
  LlmProviderFactory() = delete;

  // Creates and returns a provider instance for |provider_id|.
  // Returns nullptr if the ID is unknown.
  static std::unique_ptr<LlmProvider> CreateProvider(
      const std::string& provider_id,
      scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory);

  // Creates a provider for a user-created custom endpoint: |provider_id| is
  // `custom-<slug>`, |display_name| the user's name, |api_compatibility| the
  // dialect ("openai" or "anthropic"). No default base URL and no required API
  // key. Returns nullptr for an unrecognised dialect.
  static std::unique_ptr<LlmProvider> CreateCustomProvider(
      const std::string& provider_id,
      const std::string& display_name,
      const std::string& api_compatibility,
      scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory);

  // Returns the IDs of every registered provider, in display order.
  static std::vector<std::string> GetRegisteredProviderIds();

  // Returns metadata for every registered provider. This creates temporary
  // provider instances to collect their ModelInfo, display names, etc.
  static std::vector<ProviderInfo> GetRegisteredProviders(
      scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory);
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_PROVIDER_FACTORY_H_
