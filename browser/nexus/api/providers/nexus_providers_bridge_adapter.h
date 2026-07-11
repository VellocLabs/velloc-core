#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_PROVIDERS_NEXUS_PROVIDERS_BRIDGE_ADAPTER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_PROVIDERS_NEXUS_PROVIDERS_BRIDGE_ADAPTER_H_

#include <memory>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "custom_browser/browser/nexus/api/llm/oauth/oauth_manager.h"
#include "custom_browser/browser/nexus/api/providers/providers_api.gen.h"
#include "custom_browser/browser/nexus/bridge_response_callback.h"

class Profile;

namespace blink {
class MessagePortDescriptor;
}

namespace nexus {

class LlmBroker;
class LlmProvider;
struct ModelInfo;
class NexusBridgeResponseDelegate;

// Concrete adapter implementing the generated `ProvidersApiBase` virtuals.
//
// Approach 5 (A5): this capability is exposed ONLY on the panel WebUI port
// (`PortServiceMode::kPanel`), not on the guest-facing page/worker ports.
// Settings-page RPCs that used to ride chrome.send (`getAgentProviders`,
// `updateProviderConfig`, `verifyProviderConfig`, `refreshProviderModels`,
// `beginProviderOAuth`) migrate here one action at a time. P2 lands the
// first action: `list` (replaces `getAgentProviders`).
//
// Threading: all actions run on UI — they touch `LlmBroker` /
// `PrefService` which are UI-thread-bound. The bridge's home thread is
// already UI so the codegen-emitted dispatcher posts these to UI without
// an extra hop.
//
// Lifetime: owned by `NexusBridgePortService` in `kPanel` mode. The
// adapter holds no per-call state — every Handle*OnUI synthesizes a fresh
// response and fires `done` synchronously (no async work in this phase).
class NexusProvidersBridgeAdapter
    : public ::nexus::generated::ProvidersApiBase {
 public:
  NexusProvidersBridgeAdapter(NexusBridgeResponseDelegate* delegate,
                              Profile* profile);
  ~NexusProvidersBridgeAdapter() override;

  NexusProvidersBridgeAdapter(const NexusProvidersBridgeAdapter&) = delete;
  NexusProvidersBridgeAdapter& operator=(const NexusProvidersBridgeAdapter&) =
      delete;

 private:
  // Override of generated::ProvidersApiBase pure virtual. Runs on UI.
  // Builds `{ providers: [...], activeProviderId }` — same shape the
  // legacy chrome.send `getAgentProviders` returns. Mirrors
  // `NexusSettingsHandler::HandleGetAgentProviders` field-for-field so
  // the WebUI client can swap transports without changing parsing logic.
  void HandleListOnUI(
      ::nexus::generated::ListInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  // Synchronous update — merges the partial `updates` dict over the
  // stored Profile pref, encrypts api_key at rest, and applies the
  // result to the broker. Returns `{ success, isConfigured, baseUrl }`
  // mirroring the legacy `HandleUpdateProviderConfig` response. The
  // existing `BroadcastEnrichedCatalog_` fan-out continues to fire
  // through the legacy handler path for now — that broadcast lives on
  // `NexusSettingsHandler` which the new adapter can't reach
  // directly. P6 will move the broadcast to the broker so this
  // duplication retires.
  void HandleUpdateOnUI(
      ::nexus::generated::UpdateInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  // Async refresh: hits /v1/models (or provider-equivalent). For OAuth
  // providers, ensures a fresh access token first via the OAuth
  // manager's EnsureFreshToken path — same flow the legacy
  // `HandleRefreshProviderModels` runs. Returns
  // `{ success, models, error? }` with the updated list, or `{ success:
  // false, error }` on failure.
  void HandleRefreshOnUI(
      ::nexus::generated::RefreshInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  // OAuth-gate continuation for refresh. Fired by
  // `OAuthManager::EnsureFreshToken`. Applies the fresh token
  // in-memory to the live LlmProvider (so the next /v1/models probe
  // uses it), then kicks `LlmBroker::RefreshModels`.
  void OnOAuthTokenForRefresh(::nexus::BridgeResponseCallback done,
                              std::string provider_id,
                              bool ok,
                              std::string access_token,
                              std::string account_id);

  // RefreshModels completion. Builds the `{ success, models }` reply
  // and fires `done.Success` — mirroring
  // `NexusSettingsHandler::OnProviderModelsFetched`.
  void OnRefreshModelsFetched(::nexus::BridgeResponseCallback done,
                              std::vector<nexus::ModelInfo> models);

  // Test-connection: create a TRANSIENT LlmProvider (separate from the
  // persisted one), apply the unsaved updates, probe /v1/models.
  // Returns `{success, providerId, baseUrl, verifiedAtMs, modelCount,
  // models}` or `{success:false, errorCode, error}`. Mirrors
  // `NexusSettingsHandler::HandleVerifyProviderConfig` minus the
  // PendingVerification map — ownership of the transient provider is
  // captured directly in the FetchAvailableModels callback so it
  // self-destructs when the probe completes.
  void HandleVerifyOnUI(
      ::nexus::generated::VerifyInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  // Verify-probe completion. Captures the transient provider so it
  // lives until the probe resolves, then drops it. Mirrors
  // `NexusSettingsHandler::OnVerifyProviderModelsFetched`.
  void OnVerifyModelsFetched(
      ::nexus::BridgeResponseCallback done,
      std::string provider_id,
      std::string base_url,
      std::unique_ptr<nexus::LlmProvider> transient_provider,
      std::vector<nexus::ModelInfo> models);

  // Start the OAuth flow. Two flow variants:
  //   - device-code (GitHub Copilot, Qwen): returns
  //     `{flow:'device-code', userCode, verificationUri, expiresIn}`.
  //     The WebUI shows the user code while a background poll runs.
  //   - pkce / redirect (ChatGPT Pro, Claude Pro): opens the auth URL
  //     in a NORMAL browser window so the loopback redirect is captured
  //     by OAuthRedirectThrottle. Returns `{flow:'pkce', started:true}`.
  // Mirrors `NexusSettingsHandler::HandleBeginProviderOAuth`.
  void HandleBeginOAuthOnUI(
      ::nexus::generated::BeginOAuthInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  // Device-code flow continuation: returns the user code + verification
  // URI to the WebUI and opens the verification URL in a normal window.
  void OnDeviceCodeStartedForOAuth(
      ::nexus::BridgeResponseCallback done,
      bool ok,
      nexus::NexusOAuthManager::DeviceCodeInfo info,
      std::string error);

  // Synchronous: clears the stored config dict, drops the cached
  // catalogue, unpins as active if it was, and re-emits
  // `nexusCatalogChanged` so the popup hides the now-unverified
  // provider. Mirrors `NexusSettingsHandler::HandleResetProviderConfig`.
  // Note on the catalog re-emit: `BroadcastEnrichedCatalog_` lives on
  // `NexusSettingsHandler`, not the broker — same caveat documented on
  // `HandleUpdateOnUI`. Until P6 moves the broadcast to the broker, the
  // legacy handler stays registered to keep `nexusCatalogChanged`
  // firing from the chrome.send code path too; once both code paths
  // agree on a single broadcast source, the chrome.send handler can
  // retire.
  void HandleResetOnUI(
      ::nexus::generated::ResetInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  // Synchronous: read the live OAuth connection state from the broker's
  // OAuth manager. Returns `{connected, connectedEmail?}` — same shape
  // the legacy chrome.send `getProviderOAuthStatus` returns. The WebUI
  // polls this while a sign-in is in flight (loopback or device-code).
  // No secret in the response.
  void HandleOauthStatusOnUI(
      ::nexus::generated::OauthStatusInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  // Tier 3b: pin the active provider/model selection — the quick-
  // settings popup's pick button. Mirrors
  // `NexusSettingsHandler::HandlePickNexusSelection` (the legacy
  // chrome.send `pickNexusSelection` route): validates the dict shape,
  // calls `LlmBroker::DispatchUserPick`, returns `{ok, requestId?,
  // pusherInvoked?, errorCode?, error?}`. The broker's pusher fans a
  // `session.applySelection` event to the live SharedWorker so the
  // active session rebinds without a reload.
  void HandleSetActiveOnUI(
      ::nexus::generated::SetActiveInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  raw_ptr<Profile> profile_;
  base::WeakPtrFactory<NexusProvidersBridgeAdapter> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_PROVIDERS_NEXUS_PROVIDERS_BRIDGE_ADAPTER_H_
