#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_LLM_LLM_BROKER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_LLM_LLM_BROKER_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "custom_browser/browser/nexus/llm/llm_types.h"
#include "components/keyed_service/core/keyed_service.h"

class PrefRegistrySimple;
class PrefService;

namespace network {
class SharedURLLoaderFactory;
}  // namespace network

namespace nexus {

class LlmProvider;

// Per-profile service that owns all LlmProvider instances and routes inference
// requests to the currently active provider.
//
// On construction it reads the persisted provider ID from prefs, instantiates
// all registered providers, and configures them with stored credentials. The
// active provider can be switched at any time via SetActiveProvider().
class LlmBroker : public KeyedService {
 public:
  LlmBroker(
      PrefService* prefs,
      scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory);
  ~LlmBroker() override;

  LlmBroker(const LlmBroker&) = delete;
  LlmBroker& operator=(const LlmBroker&) = delete;

  // Registers Nexus LLM-related preferences.
  static void RegisterProfilePrefs(PrefRegistrySimple* registry);

  // --- Provider access ---

  // Returns the active provider, or nullptr if none is configured.
  LlmProvider* GetActiveProvider() const;

  std::string GetActiveProviderId() const;

  // Switches the active provider. Takes effect immediately (FR-027).
  // Returns false if |provider_id| is unknown. An empty |provider_id| clears
  // the active provider selection.
  bool SetActiveProvider(const std::string& provider_id);

  // Returns metadata for every registered provider (including whether each
  // is configured and its model catalogue).
  std::vector<ProviderInfo> GetRegisteredProviders() const;

  // Returns a raw pointer to the provider with |provider_id|, or nullptr.
  LlmProvider* GetProvider(const std::string& provider_id) const;

  // --- Configuration ---

  // Applies configuration to a provider (API key, base URL, model, etc.).
  // Returns true if the provider accepted the configuration.
  bool ConfigureProvider(const std::string& provider_id,
                         const base::DictValue& config);

  // Triggers FetchAvailableModels() on the specified provider.
  void RefreshModels(const std::string& provider_id,
                     FetchModelsCallback callback);

  // Triggers FetchAvailableModels() on the specified provider and persists the
  // raw JSON response to `kNexusProviderCatalogs` on completion. Safe to call
  // even if the provider is not configured (it's a no-op). Use this after
  // `ConfigureProvider()` so the prefs cache reflects the provider's catalogue
  // without waiting for the next browser restart.
  void RefreshCatalogForProvider(const std::string& provider_id);

  // Recreates provider instances from the persisted prefs. This is used when
  // settings reset needs to clear in-memory provider state as well.
  void ReloadProvidersFromPrefs();

  // --- Inference (delegates to active provider) ---

  void DoGenerate(const base::DictValue& call_options,
                  GenerateCallback callback);

  void DoStream(const base::DictValue& call_options,
                StreamChunkCallback chunk_callback,
                StreamDoneCallback done_callback);

  void CancelRequest(const std::string& request_id);

  // KeyedService:
  void Shutdown() override;

 private:
  void InitializeProviders();
  void LoadProviderConfigs();

  void OnCatalogRefreshed(const std::string& provider_id,
                          std::vector<ModelInfo> models);

  raw_ptr<PrefService> prefs_;
  scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory_;

  // Keyed by provider ID.
  std::map<std::string, std::unique_ptr<LlmProvider>> providers_;
  std::string active_provider_id_;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<LlmBroker> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_LLM_LLM_BROKER_H_
