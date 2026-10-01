#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_BROKER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_BROKER_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/values.h"
#include "custom_browser/browser/nexus/api/llm/llm_types.h"
#include "components/keyed_service/core/keyed_service.h"

class PrefRegistrySimple;
class PrefService;

namespace network {
class SharedURLLoaderFactory;
}  // namespace network

namespace nexus {

class LlmProvider;
class NexusOAuthManager;

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

  // --- TS-pushed catalogue + session status ---
  //
  // The curated model catalogue (`NEXUS_MODEL_CONFIG`) lives in the
  // online-updateable nexus_web bundle. nexus_web POSTs it to C++ at boot
  // via the `model.publishCatalog` bridge route; we cache it here (and in
  // prefs) because it carries the guest-owned default base URLs the
  // providers need, and because `NexusLlmHost`'s bridge catalog grafts its
  // curated models. The per-session status is cached the same way.
  void SetTsModelCatalog(base::DictValue catalog);
  const base::DictValue& GetTsModelCatalog() const;

  // Configure providers from the effective settings.json provider configs the
  // guest worker pushes via `model.publishProviderConfigs` (at boot + on every
  // providers write). Relaxed-credential path: settings.json is now the source
  // of truth for the user-editable fields (api key / base url / default model
  // / enabled models). MERGES onto the existing `nexus.provider_configs` pref
  // per provider — preserving C++-owned secrets the settings.json shape does
  // not carry (notably OAuth access/refresh tokens) — then refreshes the pref
  // so the next cold boot (`LoadProviderConfigs`) reproduces the same state.
  // `active_provider_id` (empty = leave unchanged) mirrors
  // `nexus.active_provider_id`.
  void SetProviderConfigs(base::DictValue configs,
                          const std::string& active_provider_id);

  void SetSessionStatus(base::DictValue status);
  const base::DictValue& GetSessionStatus() const;

  // --- Reverse flow: host-side pick → nexus_web ---
  //
  // The bridge adapter registers a pusher at construction. The panel
  // bridge's `providers.setActive` handler calls `DispatchUserPick`; the
  // broker validates + persists the global default, then invokes the
  // pusher so nexus_web receives a `session.applySelection` event (inert
  // today — see the DEPRECATED note in DispatchUserPick).
  using PickEventPusher = base::RepeatingCallback<void(
      const std::string& request_id,
      const std::string& provider_id,
      const std::string& model_id,
      const std::string& thinking_tier,
      bool also_update_default)>;

  // Token returned by AddPickEventPusher; pass it back to
  // RemovePickEventPusher so the broker stops fanning out events to a
  // bridge adapter that's been torn down. Multiple adapters can be
  // registered simultaneously (one per bridge port — page + worker —
  // and the broker fans every pick out to all of them, since either
  // surface might be where MultiSessionShell's event subscription
  // actually listens).
  using PickEventPusherToken = int;
  PickEventPusherToken AddPickEventPusher(PickEventPusher pusher);
  void RemovePickEventPusher(PickEventPusherToken token);

  // Result of a user-pick attempt. `error_code` is one of:
  //   "" (empty)              — success
  //   "PROVIDER_NOT_CONFIGURED"
  //   "UNKNOWN_PROVIDER"
  //   "INTERNAL"
  // Out-of-line ctor/dtor satisfy chromium-style: any struct with
  // non-trivial members (std::string here) must define them out of line.
  struct DispatchPickResult {
    DispatchPickResult();
    ~DispatchPickResult();
    DispatchPickResult(const DispatchPickResult&);
    DispatchPickResult& operator=(const DispatchPickResult&);
    DispatchPickResult(DispatchPickResult&&);
    DispatchPickResult& operator=(DispatchPickResult&&);

    bool ok = false;
    std::string error_code;
    std::string error_message;
    std::string request_id;  // populated on success — for the WebUI
                             // pending-state correlation.
    // Diagnostic: was a pusher registered when this pick fired? When
    // false, nexus_web never receives the rebind event because the
    // bridge port adapter that owns the pusher isn't alive yet (the
    // guest webview hasn't completed GuestReady, or it's been torn
    // down). Surfaced as `pusherInvoked` in `providers.setActive`'s
    // response.
    bool pusher_invoked = false;
  };
  DispatchPickResult DispatchUserPick(const std::string& provider_id,
                                      const std::string& model_id,
                                      const std::string& thinking_tier);

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

  // Ids of the providers created from USER-CREATED custom-provider configs
  // (see EnsureCustomProvider), in creation order. Empty on a profile with no
  // custom providers, which is why nothing else in the browser needs to know
  // they exist.
  const std::vector<std::string>& dynamic_provider_ids() const {
    return dynamic_provider_ids_;
  }

  // OAuth lifecycle for subscription-backed providers (ChatGPT Plus/Pro, …).
  // Owned by the broker so it shares the profile's prefs + URL loader factory.
  NexusOAuthManager* oauth_manager() const { return oauth_manager_.get(); }

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

  // True when any provider still holds `request_id` in its concurrency FIFO
  // (accepted, never dispatched upstream). Mirrors CancelRequest's broadcast:
  // the owning provider is not tracked here. Used by NexusLlmHost to tell
  // "waiting for a slot" apart from "the provider went silent" before it
  // fails a read on its idle deadline.
  bool IsRequestQueued(const std::string& request_id) const;

  // KeyedService:
  void Shutdown() override;

 private:
  void InitializeProviders();
  void LoadProviderConfigs();
  // Pushes each provider's catalog `defaultBaseUrl` into the live provider.
  void ApplyCatalogDefaultBaseUrls();
  // Once per profile, blanks `base_url` values that are only a stamp of a
  // legacy compiled-in default (see ClearBakedDefaultBaseUrls).
  void MigrateBakedDefaultBaseUrls();

  // Materialise the provider a USER-CREATED custom endpoint needs.
  //
  // The factory can only build compiled-in ids, so a provider the user added
  // in Settings would be unroutable (`GetProvider` → nullptr → the request
  // dies). Its config carries everything needed to build one — the dialect
  // (`api_compatibility`) and the user's name — so we instantiate it on
  // demand, from BOTH config paths: the guest's `publishProviderConfigs`
  // (SetProviderConfigs) and the cold-boot pref replay (LoadProviderConfigs).
  //
  // No-op (returns false) when `config` is not a custom provider, so passing
  // every config through this is safe. Idempotent for an unchanged provider;
  // a config whose DIALECT changed replaces the instance, because the wire
  // format is bound at construction — without that, flipping a saved provider
  // from OpenAI to Anthropic would keep speaking the old format.
  //
  // Removal needs nothing here: deleting a provider clears its
  // `kNexusProviderConfigs` entry and calls ReloadProvidersFromPrefs(), which
  // rebuilds this map from what survived.
  bool EnsureCustomProvider(const std::string& provider_id,
                            const base::DictValue& config);

  // Invoked by the OAuth manager after tokens are persisted; reconfigures the
  // matching in-memory provider from prefs so IsConfigured() reflects the new
  // credentials immediately.
  void OnOAuthTokensSaved(const std::string& provider_id);

  void OnCatalogRefreshed(const std::string& provider_id,
                          std::vector<ModelInfo> models);

  // OAuth providers need a backend-ready token before model discovery: the
  // stored token may be expired (chatgpt-pro/qwen) or not directly usable
  // (github-copilot's GitHub token must be exchanged for a Copilot session
  // token). RefreshModels routes OAuth providers through EnsureFreshToken →
  // OnEnsureTokenBeforeRefresh (applies the token) → FetchModelsWithPersist,
  // so every discovery path (cold-boot guest catalog, settings connect test)
  // hits the backend with a valid bearer.
  void OnEnsureTokenBeforeRefresh(const std::string& provider_id,
                                  FetchModelsCallback callback,
                                  bool ok,
                                  std::string access_token,
                                  std::string account_id);
  void FetchModelsWithPersist(const std::string& provider_id,
                              FetchModelsCallback callback);

  raw_ptr<PrefService> prefs_;
  scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory_;

  // Keyed by provider ID.
  std::map<std::string, std::unique_ptr<LlmProvider>> providers_;
  std::string active_provider_id_;

  // The subset of `providers_` built by EnsureCustomProvider, in creation
  // order — `providers_` is a std::map, so it cannot answer "which of these
  // are user-created, and in what order do I show them". GetRegisteredProviders
  // appends these AFTER the factory's fixed display order.
  std::vector<std::string> dynamic_provider_ids_;
  // Dialect each dynamic provider was built with, so a config whose
  // `api_compatibility` changed can be detected and rebuilt.
  std::map<std::string, std::string> dynamic_provider_dialects_;

  std::unique_ptr<NexusOAuthManager> oauth_manager_;

  // In-memory mirrors of the prefs-backed caches. Warm-loaded from prefs
  // at construction and overwritten by SetTsModelCatalog /
  // SetSessionStatus.
  base::DictValue ts_model_catalog_;
  base::DictValue session_status_;

  // Pushers registered by every live bridge adapter (one per bridge
  // port — page + worker). DispatchUserPick fans events out to all of
  // them so whichever surface MultiSessionShell happens to subscribe
  // on receives the rebind. Empty until at least one adapter has come
  // up; pre-bootstrap picks are still durable in prefs.
  std::map<PickEventPusherToken, PickEventPusher> pick_event_pushers_;
  PickEventPusherToken next_pusher_token_ = 0;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<LlmBroker> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_BROKER_H_
