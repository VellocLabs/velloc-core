// Copyright 2024 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

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
#include "custom_browser/browser/nexus/llm/llm_types.h"

class Profile;

namespace nexus {
class LlmBroker;
class LlmProvider;
}  // namespace nexus

namespace settings {

// WebUI message handler for the chrome://settings/nexus subpage.
//
// Follows the GlicHandler pattern: registered as a SettingsPageUIHandler,
// receives messages from the Polymer settings UI, and delegates to the Nexus
// LlmBroker KeyedService for provider management.
//
// Handler methods exposed to the settings WebUI:
//   - getAgentProviders         → returns provider list + active provider
//   - updateProviderConfig      → merges and persists provider settings
//   - verifyProviderConfig      → verifies unsaved credentials and fetches
//                                 models without persisting them
//   - resetProviderConfig       → clears saved provider settings
//   - refreshProviderModels     → fetches dynamic models for any provider
//   - setProviderApiKey         → encrypts and stores an API key
//   - validateProviderConnection → tests connectivity to a provider
//   - getOllamaModels           → fetches model list from a running Ollama
//   - setActiveProvider         → switches the active LLM provider
//   - getNexusViewState         → reports whether LlmBroker has an active
//                                 provider for the embedded Nexus view
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

  // Fetches available models for any provider using its current config.
  // JS: cr.sendWithPromise('refreshProviderModels', providerId)
  void HandleRefreshProviderModels(const base::ListValue& args);

  // Encrypts and stores an API key for a specific provider.
  // JS: cr.sendWithPromise('setProviderApiKey', providerId, apiKey)
  void HandleSetProviderApiKey(const base::ListValue& args);

  // Tests connectivity to a provider using its current credentials.
  // JS: cr.sendWithPromise('validateProviderConnection', providerId)
  void HandleValidateProviderConnection(const base::ListValue& args);

  // Fetches available models from a running Ollama instance.
  // JS: cr.sendWithPromise('getOllamaModels', baseUrl)
  void HandleGetOllamaModels(const base::ListValue& args);

  // Switches the active LLM provider (immediate, no restart needed).
  // JS: cr.sendWithPromise('setActiveProvider', providerId)
  void HandleSetActiveProvider(const base::ListValue& args);

  // Returns whether the current profile has an active LLM provider selected.
  // JS: cr.sendWithPromise('getNexusViewState')
  void HandleGetNexusViewState(const base::ListValue& args);

  // Callback for asynchronous Ollama model fetch.
  void OnOllamaModelsFetched(const std::string& callback_id,
                             std::vector<nexus::ModelInfo> models);

  // Callback for asynchronous generic provider model fetch.
  void OnProviderModelsFetched(const std::string& callback_id,
                               std::vector<nexus::ModelInfo> models);

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
