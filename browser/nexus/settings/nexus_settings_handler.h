// Copyright 2024 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_SETTINGS_NEXUS_SETTINGS_HANDLER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_SETTINGS_NEXUS_SETTINGS_HANDLER_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/ui/webui/settings/settings_page_ui_handler.h"
#include "custom_browser/browser/nexus/api/llm/llm_types.h"
#include "custom_browser/browser/nexus/api/llm/oauth/oauth_manager.h"

class Profile;

namespace nexus {
class LlmBroker;
class LlmProvider;
}  // namespace nexus

namespace settings {

// WebUI message handler for the chrome://settings/nexus subpage, delegating to
// the LlmBroker KeyedService. chrome.send routes: provider CRUD (used directly
// by the e2e harness), get/setWebappUrl, restartBrowser, reloadPanel, and the
// background-mode / launcher local_state prefs.
class NexusSettingsHandler : public SettingsPageUIHandler {
 public:
  NexusSettingsHandler();

  NexusSettingsHandler(const NexusSettingsHandler&) = delete;
  NexusSettingsHandler& operator=(const NexusSettingsHandler&) = delete;

  ~NexusSettingsHandler() override;

  // SettingsPageUIHandler:
  void RegisterMessages() override;
  void OnJavascriptAllowed() override;
  void OnJavascriptDisallowed() override;

 private:
  // Returns the LlmBroker for the current profile. May return nullptr
  // if the profile is not valid (e.g. incognito, sign-in profile).
  nexus::LlmBroker* GetBroker();

  // Returns the current profile from the WebUI's WebContents.
  Profile* GetProfile();

  // --- Handler methods ---

  // Returns all registered providers with their configuration status
  // and the currently active provider ID.
  // JS: cr.sendWithPromise('getAgentProviders')
  void HandleGetAgentProviders(const base::ListValue& args);

  // Merges provider config updates (API key, base URL, model, etc.) with the
  // stored config and applies them immediately.
  // JS: cr.sendWithPromise('updateProviderConfig', providerId, config)
  void HandleUpdateProviderConfig(const base::ListValue& args);

  // Verifies a provider connection using unsaved form data and then fetches
  // the provider's current model catalogue.
  // JS: cr.sendWithPromise('verifyProviderConfig', providerId, config)
  void HandleVerifyProviderConfig(const base::ListValue& args);

  // Clears the stored config for a provider and resets the active provider if
  // it points at the removed entry.
  // JS: cr.sendWithPromise('resetProviderConfig', providerId)
  void HandleResetProviderConfig(const base::ListValue& args);


  // HandleSetProviderApiKey, HandleValidateProviderConnection,
  // HandleGetOllamaModels, HandleSetActiveProvider, HandleGetNexusViewState
  // declarations removed — all five were vapor (registered + impl'd
  // but never called from JS). See nexus_settings_handler.cc for the
  // per-method removal rationale.

  // --- Custom Nexus web app URL ---
  // Read/written here because chrome.settingsPrivate is not exposed to
  // chrome://custom.top-chrome. Returns the persisted URL (empty = default).
  // JS: cr.sendWithPromise('getWebappUrl')
  void HandleGetWebappUrl(const base::ListValue& args);

  // Persists the custom web app URL. An empty string clears the override.
  // The value is applied on the next browser launch by
  // nexus::GetResolvedNexusGuestURL(), which re-validates it.
  // JS: cr.sendWithPromise('setWebappUrl', url)
  void HandleSetWebappUrl(const base::ListValue& args);

  // Restarts the client, restoring the open session (nexus::RestartClient).
  // Backs the guest-recovery overlay's "Restart client" button: the overlay
  // paints in the HOST page exactly when the guest never completed its
  // handshake, so the guest bridge's `appInfo.relaunch` — which the About
  // page uses for the same job — is unreachable from there by construction.
  // Fire-and-forget: there is no reply to wait for, since the browser is on
  // its way out.
  // JS: chrome.send('restartBrowser')
  void HandleRestartBrowser(const base::ListValue& args);

  // Reloads this WebUI page from the browser side. Backs the guest-recovery
  // overlay's "Try again" and its retry on `online`: the page's own
  // `location.reload()` is aborted on chrome://custom.top-chrome — the
  // guest <webview> reports ERR_ABORTED and the document never goes away —
  // while a browser-initiated reload of the same contents goes through.
  // Fire-and-forget: the page is replaced, so there is nothing to answer.
  // JS: chrome.send('reloadPanel')
  void HandleReloadPanel(const base::ListValue& args);

  // HandleGet/SetBrowserToolsSeparateWindow declarations removed:
  // migrated to `browserTools.snapshot` / `browserTools.setField` on the
  // panel WebUI bridge port (A5 Tier 2). See
  // api/browserTools/nexus_browser_tools_bridge_adapter.h. The runtime
  // pref read `ShouldHostAgentTabsInSeparateWindow` is unchanged.

  // HandleGetBrowserToolsAllowEvaluate / HandleSetBrowserToolsAllowEvaluate
  // declarations removed alongside the Settings · Tools opt-in toggle.
  // BrowserEvaluate is now always available (no gate / pref); see
  // nexus_browser_tools_config.h.

  // --- System: background mode ---
  // prefs::kBackgroundModeEnabled (local_state, default true), observed by
  // BackgroundModeManager. Without ENABLE_BACKGROUND_MODE the getter reports
  // false and the setter rejects.
  // JS: cr.sendWithPromise('getBackgroundModeEnabled')
  void HandleGetBackgroundModeEnabled(const base::ListValue& args);

  // Persists the background-mode flag into local_state.
  // JS: cr.sendWithPromise('setBackgroundModeEnabled', enabled)
  void HandleSetBackgroundModeEnabled(const base::ListValue& args);

  // --- System: Nexus launcher (tray icon + keep-alive) ---
  // Aliases of the background-mode pair: both forward to
  // prefs::kBackgroundModeEnabled.
  // JS: cr.sendWithPromise('getNexusLauncherEnabled')
  void HandleGetNexusLauncherEnabled(const base::ListValue& args);

  // Persists the launcher flag into local_state.
  // JS: cr.sendWithPromise('setNexusLauncherEnabled', enabled)
  void HandleSetNexusLauncherEnabled(const base::ListValue& args);


  // OnOllamaModelsFetched removed alongside HandleGetOllamaModels.
  // OnProviderModelsFetched / OnOAuthTokenForModels removed alongside
  // HandleRefreshProviderModels (migrated to `providers.refresh`).

  // Callback for the verify-and-load flow. The transient provider fetches
  // its model catalogue as the connectivity probe, and this delivers the
  // result (or a mapped error code) back to the WebUI.
  void OnVerifyProviderModelsFetched(
      const std::string& callback_id,
      std::vector<nexus::ModelInfo> models);

  struct PendingVerification {
    PendingVerification();
    ~PendingVerification();

    PendingVerification(const PendingVerification&) = delete;
    PendingVerification& operator=(const PendingVerification&) = delete;
    PendingVerification(PendingVerification&&);
    PendingVerification& operator=(PendingVerification&&);

    std::string provider_id;
    std::string base_url;
    std::unique_ptr<nexus::LlmProvider> provider;
  };

  std::map<std::string, PendingVerification> pending_verifications_;

  base::WeakPtrFactory<NexusSettingsHandler> weak_factory_{this};
};

}  // namespace settings

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_SETTINGS_NEXUS_SETTINGS_HANDLER_H_
