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

// Creates LlmProvider instances by provider ID.
//
// Adding a new provider:
//   1. Create files in providers/{name}/ — or, for an endpoint that speaks the
//      OpenAI or Anthropic wire format, reuse OpenAiProvider / AnthropicProvider
//      via their parameterized (id, display_name, default_base_url) ctor.
//   2. Register the ID in CreateProvider() and add it to kProviderIds.
//   3. An API-key vendor's default base URL is guest-owned: set
//      `defaultBaseUrl` on its nexus_web provider config, not here.
class LlmProviderFactory {
 public:
  LlmProviderFactory() = delete;

  // Creates and returns a provider instance for |provider_id|.
  // Returns nullptr if the ID is unknown.
  static std::unique_ptr<LlmProvider> CreateProvider(
      const std::string& provider_id,
      scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory);

  // Creates a provider for a USER-CREATED custom endpoint — one the user
  // added in Settings -> Provider connection, which by definition has no
  // compiled-in ID. |provider_id| is the settings-generated `custom-<slug>`,
  // |display_name| the user's name for it, and |api_compatibility| the wire
  // dialect it speaks ("openai" or "anthropic"). Like the two built-in custom
  // endpoints, the instance ships with NO default base URL (the user supplies
  // one via Configure()) and does not require an API key — many compatible
  // gateways run keyless.
  //
  // Returns nullptr for an unrecognised dialect. The broker calls this from
  // the published provider configs; see LlmBroker::EnsureCustomProvider.
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
