#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_BROKER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_BROKER_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "base/observer_list_types.h"
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
  // Observers are notified when the TS-pushed catalogue or the session
  // status changes. Used by the WebUI settings handler to forward updates
  // to the Lit `<nexus-quick-settings>` popup via FireWebUIListener.
  class Observer : public base::CheckedObserver {
   public:
    ~Observer() override = default;

    // Fired when nexus_web publishes a new `NEXUS_MODEL_CONFIG` snapshot
    // (typically once at boot, plus any time the online TS bundle hot-
    // reloads). The payload mirrors what the TS layer sent.
    virtual void OnTsModelCatalogChanged(const base::DictValue& catalog) {}

    // Fired when nexus_web publishes a new SessionStatus — either because
    // the user switched sessions, or the current session's selection /
    // other status fields changed.
    virtual void OnSessionStatusChanged(const base::DictValue& status) {}

    // Fired after a provider's model list is (re)discovered from its backend,
    // regardless of which code path triggered the fetch (guest catalog refresh,
    // login, or a settings connect test). Lets the WebUI rebuild the topbar
    // quick-switch popup once async discovery lands — notably for OAuth
    // providers (chatgpt-pro) whose models are fetched lazily after cold boot,
    // which would otherwise stay hidden until the user opened settings.
    virtual void OnProviderModelsRefreshed(const std::string& provider_id) {}
  };

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
  // via the `model.publishCatalog` bridge route; we cache it here so the
  // browser-side WebUI quick-settings popup can render it without a
  // round-trip to the guest. Same pattern for the per-session status.
  void SetTsModelCatalog(base::DictValue catalog);
  const base::DictValue& GetTsModelCatalog() const;

  void SetSessionStatus(base::DictValue status);
  const base::DictValue& GetSessionStatus() const;

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  // --- Reverse flow: WebUI quick-settings popup → nexus_web ---
  //
  // The bridge adapter registers a pusher at construction. When the user
  // picks a model in the WebUI popup, the settings handler calls
  // `DispatchUserPick`; the broker validates + persists the global
  // default, then invokes the pusher so nexus_web receives a
  // `session.applySelection` event. nexus_web responds by rebinding the
  // active session and re-publishing its status.
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
    // down). Surfaced through `pickNexusSelection`'s response so the
    // WebUI can warn / retry instead of waiting forever for an event
    // that will never come.
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

  // KeyedService:
  void Shutdown() override;

 private:
  void InitializeProviders();
  void LoadProviderConfigs();

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

  std::unique_ptr<NexusOAuthManager> oauth_manager_;

  // In-memory mirrors of the prefs-backed caches. Populated lazily from
  // prefs on first access and overwritten by SetTsModelCatalog /
  // SetSessionStatus. Both are kept as `base::DictValue` so the WebUI
  // settings handler can forward them straight to FireWebUIListener
  // without re-parsing.
  base::DictValue ts_model_catalog_;
  base::DictValue session_status_;

  // Pushers registered by every live bridge adapter (one per bridge
  // port — page + worker). DispatchUserPick fans events out to all of
  // them so whichever surface MultiSessionShell happens to subscribe
  // on receives the rebind. Empty until at least one adapter has come
  // up; pre-bootstrap picks are still durable in prefs.
  std::map<PickEventPusherToken, PickEventPusher> pick_event_pushers_;
  PickEventPusherToken next_pusher_token_ = 0;

  base::ObserverList<Observer> observers_;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<LlmBroker> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_BROKER_H_
