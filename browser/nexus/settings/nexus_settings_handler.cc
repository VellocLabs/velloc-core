// Copyright 2024 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/settings/nexus_settings_handler.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "custom_browser/browser/nexus/api/llm/llm_broker.h"
#include "custom_browser/browser/nexus/api/llm/llm_broker_factory.h"
#include "custom_browser/browser/nexus/api/llm/llm_pref_names.h"
#include "custom_browser/browser/nexus/api/llm/llm_provider.h"
#include "custom_browser/browser/nexus/api/llm/nexus_llm_host.h"
#include "custom_browser/browser/nexus/api/llm/llm_provider_factory.h"
#include "custom_browser/browser/nexus/api/llm/llm_types.h"
#include "custom_browser/browser/nexus/api/llm/oauth/oauth_manager.h"
#include "custom_browser/browser/nexus/api/llm/oauth/oauth_providers.h"
#include "custom_browser/browser/nexus/api/llm/oauth/oauth_url_opener.h"
#include "custom_browser/browser/nexus/api/llm/provider_config.h"
#include "custom_browser/browser/nexus/nexus_browser_tools_config.h"
#include "url/gurl.h"

namespace settings {

namespace {

const char kErrorCodeAuthUnauthorized[] = "auth_unauthorized";
const char kErrorCodeDiscoveryNoModels[] = "discovery_no_models";
const char kErrorCodeSystemUnexpected[] = "system_unexpected";

bool HasStoredApiKey(const base::DictValue& config) {
  const auto* api_key = config.FindString("api_key");
  return api_key && !api_key->empty();
}

base::DictValue GetStoredProviderConfigCopy(PrefService* prefs,
                                            const std::string& provider_id) {
  const base::DictValue* config =
      nexus::GetStoredProviderConfig(prefs, provider_id);
  return config ? config->Clone() : base::DictValue();
}

base::DictValue MergeProviderConfigUpdates(PrefService* prefs,
                                           nexus::LlmProvider* provider,
                                           const std::string& provider_id,
                                           const base::DictValue& updates) {
  base::DictValue merged = GetStoredProviderConfigCopy(prefs, provider_id);

  if (const auto* api_key = updates.FindString("api_key");
      api_key && !api_key->empty()) {
    merged.Set("api_key", *api_key);
  }

  if (const auto* account_token = updates.FindString("account_token");
      account_token && !account_token->empty()) {
    merged.Set("account_token", *account_token);
  }

  if (const auto* model = updates.FindString("model");
      model && !model->empty()) {
    merged.Set("model", *model);
  }

  if (const auto* default_model = updates.FindString("default_model")) {
    if (!default_model->empty()) {
      merged.Set("default_model", *default_model);
      merged.Set("model", *default_model);
    } else {
      merged.Remove("default_model");
      merged.Remove("model");
    }
  }

  if (updates.Find("enabled_models")) {
    if (const auto* enabled_models = updates.FindList("enabled_models");
        enabled_models && !enabled_models->empty()) {
      merged.Set("enabled_models", enabled_models->Clone());
    } else {
      merged.Remove("enabled_models");
    }
  }

  if (const auto* last_verification = updates.FindDict("last_verification")) {
    merged.Set("last_verification", last_verification->Clone());
  }

  if (updates.Find("base_url")) {
    const std::string* base_url = updates.FindString("base_url");
    if (base_url && !base_url->empty()) {
      merged.Set("base_url", *base_url);
    } else if (provider) {
      merged.Set("base_url", provider->GetDefaultBaseUrl());
    } else {
      merged.Set("base_url", std::string());
    }
  }

  return merged;
}

base::DictValue BuildVerificationConfig(PrefService* prefs,
                                        nexus::LlmProvider* provider,
                                        const std::string& provider_id,
                                        const base::DictValue& updates) {
  base::DictValue merged =
      MergeProviderConfigUpdates(prefs, provider, provider_id, updates);
  merged.Remove("model");
  merged.Remove("default_model");
  merged.Remove("enabled_models");
  merged.Remove("last_verification");
  return merged;
}

base::ListValue BuildModelList(const std::vector<nexus::ModelInfo>& models) {
  base::ListValue model_list;
  for (const auto& model : models) {
    base::DictValue model_dict;
    model_dict.Set("name", model.name);
    model_dict.Set("id", model.id);
    model_dict.Set("contextWindow", model.context_window);
    model_dict.Set("supportsTools", model.supports_tools);
    model_dict.Set("supportsVision", model.supports_vision);
    model_list.Append(std::move(model_dict));
  }
  return model_list;
}

base::DictValue BuildValidationCallOptions(const std::string& model_name) {
  base::DictValue call_options;
  call_options.Set("model", model_name);
  call_options.Set("modelId", model_name);
  call_options.Set("inputFormat", "messages");

  base::ListValue prompt;
  base::DictValue message;
  message.Set("role", "user");
  base::ListValue content;
  base::DictValue text_part;
  text_part.Set("type", "text");
  text_part.Set("text", "Say hello.");
  content.Append(std::move(text_part));
  message.Set("content", content.Clone());
  prompt.Append(message.Clone());

  call_options.Set("prompt", std::move(prompt));

  base::ListValue messages;
  messages.Append(std::move(message));
  call_options.Set("messages", std::move(messages));

  base::DictValue mode;
  mode.Set("type", "regular");
  call_options.Set("mode", std::move(mode));
  call_options.Set("maxTokens", 5);
  call_options.Set("max_tokens", 5);

  return call_options;
}

std::string GetConfiguredModelName(PrefService* prefs,
                                   const std::string& provider_id,
                                   nexus::LlmProvider* provider) {
  base::DictValue config = GetStoredProviderConfigCopy(prefs, provider_id);
  const std::string stored_default_model = nexus::GetStoredDefaultModelId(config);
  if (!stored_default_model.empty()) {
    return stored_default_model;
  }
  return provider ? provider->GetDefaultModelId() : std::string();
}

std::string GetConfiguredBaseUrl(PrefService* prefs,
                                 const std::string& provider_id,
                                 nexus::LlmProvider* provider) {
  base::DictValue config = GetStoredProviderConfigCopy(prefs, provider_id);
  if (const auto* base_url = config.FindString("base_url");
      base_url && !base_url->empty()) {
    return *base_url;
  }

  return provider ? provider->GetBaseUrl() : std::string();
}

std::string GetConfiguredBaseUrlFromConfig(const base::DictValue& config,
                                           nexus::LlmProvider* provider) {
  if (const auto* base_url = config.FindString("base_url");
      base_url && !base_url->empty()) {
    return *base_url;
  }

  return provider ? provider->GetBaseUrl() : std::string();
}

base::DictValue BuildVerificationFailure(std::string error,
                                         std::string error_code) {
  base::DictValue result;
  result.Set("success", false);
  result.Set("error", error);
  result.Set("errorCode", std::move(error_code));
  return result;
}

}  // namespace

NexusSettingsHandler::PendingVerification::PendingVerification() = default;
NexusSettingsHandler::PendingVerification::~PendingVerification() = default;
NexusSettingsHandler::PendingVerification::PendingVerification(
    PendingVerification&&) = default;
NexusSettingsHandler::PendingVerification&
NexusSettingsHandler::PendingVerification::operator=(PendingVerification&&) =
    default;

NexusSettingsHandler::NexusSettingsHandler() = default;
NexusSettingsHandler::~NexusSettingsHandler() = default;

void NexusSettingsHandler::RegisterMessages() {
  web_ui()->RegisterMessageCallback(
      "getAgentProviders",
      base::BindRepeating(&NexusSettingsHandler::HandleGetAgentProviders,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "getNexusViewState",
      base::BindRepeating(&NexusSettingsHandler::HandleGetNexusViewState,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "updateProviderConfig",
      base::BindRepeating(&NexusSettingsHandler::HandleUpdateProviderConfig,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "verifyProviderConfig",
      base::BindRepeating(&NexusSettingsHandler::HandleVerifyProviderConfig,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "resetProviderConfig",
      base::BindRepeating(&NexusSettingsHandler::HandleResetProviderConfig,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "refreshProviderModels",
      base::BindRepeating(&NexusSettingsHandler::HandleRefreshProviderModels,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "setProviderApiKey",
      base::BindRepeating(&NexusSettingsHandler::HandleSetProviderApiKey,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "validateProviderConnection",
      base::BindRepeating(
          &NexusSettingsHandler::HandleValidateProviderConnection,
          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "getOllamaModels",
      base::BindRepeating(&NexusSettingsHandler::HandleGetOllamaModels,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "setActiveProvider",
      base::BindRepeating(&NexusSettingsHandler::HandleSetActiveProvider,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "beginProviderOAuth",
      base::BindRepeating(&NexusSettingsHandler::HandleBeginProviderOAuth,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "getProviderOAuthStatus",
      base::BindRepeating(&NexusSettingsHandler::HandleGetProviderOAuthStatus,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "getWebappUrl",
      base::BindRepeating(&NexusSettingsHandler::HandleGetWebappUrl,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "setWebappUrl",
      base::BindRepeating(&NexusSettingsHandler::HandleSetWebappUrl,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "getBrowserToolsSeparateWindow",
      base::BindRepeating(
          &NexusSettingsHandler::HandleGetBrowserToolsSeparateWindow,
          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "setBrowserToolsSeparateWindow",
      base::BindRepeating(
          &NexusSettingsHandler::HandleSetBrowserToolsSeparateWindow,
          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "getAgentConfig",
      base::BindRepeating(&NexusSettingsHandler::HandleGetAgentConfig,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "setAgentConfig",
      base::BindRepeating(&NexusSettingsHandler::HandleSetAgentConfig,
                          base::Unretained(this)));
  // Quick-settings popup support — read-side handlers + the user-pick
  // write-side handler. See nexus_settings_handler.h for full contracts.
  web_ui()->RegisterMessageCallback(
      "getNexusCatalog",
      base::BindRepeating(&NexusSettingsHandler::HandleGetNexusCatalog,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "getNexusSessionStatus",
      base::BindRepeating(&NexusSettingsHandler::HandleGetNexusSessionStatus,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "pickNexusSelection",
      base::BindRepeating(&NexusSettingsHandler::HandlePickNexusSelection,
                          base::Unretained(this)));
}

void NexusSettingsHandler::OnJavascriptAllowed() {
  // Subscribe to broker change signals so we can fan them out to the
  // Lit `<nexus-quick-settings>` popup. The observation only fires while
  // JS is allowed; if JS gets disabled (page reload mid-session) we
  // tear down in OnJavascriptDisallowed.
  if (nexus::LlmBroker* broker = GetBroker()) {
    broker_observation_.Observe(broker);
  }
}

void NexusSettingsHandler::OnJavascriptDisallowed() {
  broker_observation_.Reset();
  pending_verifications_.clear();
  weak_factory_.InvalidateWeakPtrs();
}

void NexusSettingsHandler::OnTsModelCatalogChanged(
    const base::DictValue& /*catalog*/) {
  // The popup wants the *enriched* catalog (with iconSvg, isVerified,
  // etc.), not the raw TS-pushed dict. Rebuild it the same way
  // HandleGetNexusCatalog does and broadcast.
  BroadcastEnrichedCatalog_();
}

void NexusSettingsHandler::OnSessionStatusChanged(
    const base::DictValue& /*status*/) {
  // The model selection is now broadcast as part of the unified
  // `nexusCurrentSessionState` event sourced from the SharedWorker's
  // session manager (see `SharedWorkerSessionManager.setModel`). The
  // legacy `nexusSessionStatusChanged` fan-out is retired; the broker's
  // session-status cache is still updated for any non-WebUI consumer
  // that reads it via `LlmBroker::GetSessionStatus`.
}

void NexusSettingsHandler::OnProviderModelsRefreshed(
    const std::string& /*provider_id*/) {
  // A provider's model list was just discovered from its backend (the guest's
  // cold-boot catalog refresh, a login, or a connect test). Rebuild the
  // always-mounted topbar quick-switch popup so lazily-discovered OAuth models
  // (chatgpt-pro) appear immediately — without this the popup keeps the empty
  // catalog it captured before discovery finished, and the only way to refresh
  // it was to open the provider settings page.
  BroadcastEnrichedCatalog_();
}

Profile* NexusSettingsHandler::GetProfile() {
  return Profile::FromWebUI(web_ui());
}

nexus::LlmBroker* NexusSettingsHandler::GetBroker() {
  Profile* profile = GetProfile();
  if (!profile) {
    return nullptr;
  }
  return nexus::LlmBrokerFactory::GetForProfile(profile);
}

// --- Handler Methods ---------------------------------------------------------

void NexusSettingsHandler::HandleGetAgentProviders(
    const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 1u);
  const std::string& callback_id = args[0].GetString();

  Profile* profile = GetProfile();
  nexus::LlmBroker* broker = GetBroker();
  if (!profile || !broker) {
    RejectJavascriptCallback(
        base::Value(callback_id),
        base::Value("Nexus LLM Broker is not available"));
    return;
  }

  PrefService* prefs = profile->GetPrefs();
  std::vector<nexus::ProviderInfo> providers = broker->GetRegisteredProviders();
  std::string active_id = broker->GetActiveProviderId();

  base::ListValue provider_list;
  for (const auto& info : providers) {
    nexus::LlmProvider* provider = broker->GetProvider(info.id);
    base::DictValue stored_config = GetStoredProviderConfigCopy(prefs, info.id);
    base::DictValue provider_dict;
    provider_dict.Set("id", info.id);
    provider_dict.Set("name", info.name);

    std::string type_string;
    switch (info.type) {
      case nexus::LlmProviderType::kCloud:
        type_string = "cloud";
        break;
      case nexus::LlmProviderType::kLocal:
        type_string = "local";
        break;
      case nexus::LlmProviderType::kProxy:
        type_string = "proxy";
        break;
    }
    provider_dict.Set("type", type_string);
    provider_dict.Set("isConfigured", info.is_configured);
    provider_dict.Set("hasStoredApiKey", HasStoredApiKey(stored_config));

    // OAuth-backed providers (ChatGPT Plus/Pro) render a "Sign in" card
    // instead of base-url/api-key fields. Surface the auth mode and live
    // connection status so the WebUI can pick the right UI.
    const bool is_oauth = nexus::IsOAuthProvider(info.id);
    provider_dict.Set("authMode", is_oauth ? "oauth" : "api_key");
    if (is_oauth && broker->oauth_manager()) {
      provider_dict.Set("connected",
                        broker->oauth_manager()->IsConnected(info.id));
      const std::string email =
          broker->oauth_manager()->GetConnectedEmail(info.id);
      if (!email.empty()) {
        provider_dict.Set("connectedEmail", email);
      }
    }

    const std::string configured_model =
        GetConfiguredModelName(prefs, info.id, provider);
    if (!configured_model.empty()) {
      provider_dict.Set("model", configured_model);
    }

    const std::string stored_default_model =
        nexus::GetStoredDefaultModelId(stored_config);
    if (!stored_default_model.empty()) {
      provider_dict.Set("defaultModel", stored_default_model);
    }

    const std::string configured_base_url =
        GetConfiguredBaseUrl(prefs, info.id, provider);
    if (!configured_base_url.empty()) {
      provider_dict.Set("baseUrl", configured_base_url);
    }

    if (const auto* enabled_models = stored_config.FindList("enabled_models")) {
      provider_dict.Set("enabledModels", enabled_models->Clone());
    }

    if (const auto* last_verification =
            stored_config.FindDict("last_verification")) {
      provider_dict.Set("lastVerification", last_verification->Clone());
    }

    provider_dict.Set("models", BuildModelList(info.models));

    provider_list.Append(std::move(provider_dict));
  }

  base::DictValue result;
  result.Set("providers", std::move(provider_list));
  result.Set("activeProviderId", active_id);

  ResolveJavascriptCallback(base::Value(callback_id),
                            std::move(result));
}

void NexusSettingsHandler::HandleGetNexusViewState(
    const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 1u);
  const std::string& callback_id = args[0].GetString();

  nexus::LlmBroker* broker = GetBroker();

  base::DictValue result;
  result.Set("hasActiveProvider",
             broker && broker->GetActiveProvider() != nullptr);
  result.Set("activeProviderId",
             broker ? broker->GetActiveProviderId() : std::string());

  ResolveJavascriptCallback(base::Value(callback_id), std::move(result));
}

void NexusSettingsHandler::HandleUpdateProviderConfig(
    const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 3u);
  const std::string& callback_id = args[0].GetString();
  const std::string& provider_id = args[1].GetString();

  const base::DictValue* updates = args[2].GetIfDict();
  if (!updates) {
    RejectJavascriptCallback(base::Value(callback_id),
                             base::Value("Provider config update must be a "
                                         "dictionary"));
    return;
  }

  Profile* profile = GetProfile();
  nexus::LlmBroker* broker = GetBroker();
  if (!profile || !broker) {
    RejectJavascriptCallback(
        base::Value(callback_id),
        base::Value("Nexus LLM Broker is not available"));
    return;
  }

  nexus::LlmProvider* provider = broker->GetProvider(provider_id);
  if (!provider) {
    base::DictValue result;
    result.Set("success", false);
    result.Set("error", "Unknown provider: " + provider_id);
    ResolveJavascriptCallback(base::Value(callback_id), std::move(result));
    return;
  }

  PrefService* prefs = profile->GetPrefs();
  base::DictValue merged_config =
      MergeProviderConfigUpdates(prefs, provider, provider_id, *updates);
  const bool success = broker->ConfigureProvider(provider_id, merged_config);

  if (success && provider->IsConfigured()) {
    // Populate the prefs-backed catalogue cache for this provider now that we
    // have credentials. The hard-coded TS config
    // (`nexus_web/src/browser/config/nexusModelConfig.ts`) is authoritative
    // for the model list shown in the UI, so this fetch is best-effort — it
    // lets the Settings UI cross-check that the API key works and surface
    // any provider-side model metadata we might want later.
    broker->RefreshCatalogForProvider(provider_id);

    // First-time onboarding: if this is the first configured provider in the
    // profile, promote it to active automatically. Otherwise the user has to
    // make a second trip into settings (or through the provider picker) just
    // to flip the active flag before anything works.
    if (broker->GetActiveProviderId().empty()) {
      broker->SetActiveProvider(provider_id);
    }
  }

  // Re-emit the enriched catalog so the WebUI quick-settings popup sees
  // the new isVerified / enabledModels / baseUrl state without waiting
  // for the next open.
  BroadcastEnrichedCatalog_();

  base::DictValue result;
  result.Set("success", success);
  result.Set("isConfigured", provider->IsConfigured());
  result.Set("baseUrl", provider->GetBaseUrl());
  if (!success) {
    result.Set("error", "Failed to configure provider: " + provider_id);
  }

  ResolveJavascriptCallback(base::Value(callback_id), std::move(result));
}

void NexusSettingsHandler::HandleVerifyProviderConfig(
    const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 3u);
  const std::string& callback_id = args[0].GetString();
  const std::string& provider_id = args[1].GetString();

  const base::DictValue* updates = args[2].GetIfDict();
  if (!updates) {
    RejectJavascriptCallback(base::Value(callback_id),
                             base::Value("Provider verification config must "
                                         "be a dictionary"));
    return;
  }

  Profile* profile = GetProfile();
  nexus::LlmBroker* broker = GetBroker();
  if (!profile || !broker) {
    RejectJavascriptCallback(
        base::Value(callback_id),
        base::Value("Nexus LLM Broker is not available"));
    return;
  }

  nexus::LlmProvider* persisted_provider = broker->GetProvider(provider_id);
  if (!persisted_provider) {
    ResolveJavascriptCallback(
        base::Value(callback_id),
        BuildVerificationFailure("Unknown provider: " + provider_id,
                                 kErrorCodeSystemUnexpected));
    return;
  }

  auto transient_provider = nexus::LlmProviderFactory::CreateProvider(
      provider_id,
      profile->GetDefaultStoragePartition()->GetURLLoaderFactoryForBrowserProcess());
  if (!transient_provider) {
    ResolveJavascriptCallback(
        base::Value(callback_id),
        BuildVerificationFailure("Failed to create provider: " + provider_id,
                                 kErrorCodeSystemUnexpected));
    return;
  }

  base::DictValue verification_config = BuildVerificationConfig(
      profile->GetPrefs(), persisted_provider, provider_id, *updates);
  if (!transient_provider->Configure(verification_config)) {
    ResolveJavascriptCallback(
        base::Value(callback_id),
        BuildVerificationFailure(
            "Provider is not configured. Check the required connection "
            "fields and try again.",
            kErrorCodeAuthUnauthorized));
    return;
  }

  PendingVerification pending;
  pending.provider_id = provider_id;
  pending.base_url = GetConfiguredBaseUrlFromConfig(
      verification_config, transient_provider.get());
  pending.provider = std::move(transient_provider);

  pending_verifications_.erase(callback_id);
  auto emplace_result =
      pending_verifications_.emplace(callback_id, std::move(pending));
  auto it = emplace_result.first;

  // Use the /v1/models (or provider-equivalent) endpoint as the connectivity
  // probe. This exercises the same auth surface as a generation call, returns
  // well-defined error codes on failure, and is the data we actually need
  // next — no reason to make a separate throw-away `DoGenerate` call with a
  // synthetic "hello" prompt. Earlier versions of this handler did a dummy
  // DoGenerate first, but `GetDefaultModelId()` now returns empty by design
  // (the TS layer owns model selection), so a generation probe can never
  // succeed here.
  it->second.provider->FetchAvailableModels(
      base::BindOnce(&NexusSettingsHandler::OnVerifyProviderModelsFetched,
                     weak_factory_.GetWeakPtr(), callback_id));
}

void NexusSettingsHandler::HandleResetProviderConfig(
    const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 2u);
  const std::string& callback_id = args[0].GetString();
  const std::string& provider_id = args[1].GetString();

  Profile* profile = GetProfile();
  nexus::LlmBroker* broker = GetBroker();
  if (!profile || !broker) {
    RejectJavascriptCallback(
        base::Value(callback_id),
        base::Value("Nexus LLM Broker is not available"));
    return;
  }

  if (!broker->GetProvider(provider_id)) {
    base::DictValue result;
    result.Set("success", false);
    result.Set("error", "Unknown provider: " + provider_id);
    ResolveJavascriptCallback(base::Value(callback_id), std::move(result));
    return;
  }

  PrefService* prefs = profile->GetPrefs();
  base::DictValue all_configs =
      prefs->GetDict(nexus::prefs::kNexusProviderConfigs).Clone();
  all_configs.Remove(provider_id);
  prefs->SetDict(nexus::prefs::kNexusProviderConfigs, std::move(all_configs));

  // Drop this provider's cached catalogue so a reset truly clears every trace
  // of it — otherwise the next browser start would still seed the provider's
  // in-memory catalogue from prefs before it gets a chance to refetch.
  base::DictValue all_catalogs =
      prefs->GetDict(nexus::prefs::kNexusProviderCatalogs).Clone();
  all_catalogs.Remove(provider_id);
  prefs->SetDict(nexus::prefs::kNexusProviderCatalogs, std::move(all_catalogs));

  if (broker->GetActiveProviderId() == provider_id) {
    broker->SetActiveProvider(std::string());
  }
  broker->ReloadProvidersFromPrefs();

  base::DictValue result;
  result.Set("success", true);
  result.Set("activeProviderId", broker->GetActiveProviderId());
  ResolveJavascriptCallback(base::Value(callback_id), std::move(result));

  // The reset cleared verification + enabled-models — re-emit so the
  // popup hides the now-unverified provider without a re-open.
  BroadcastEnrichedCatalog_();
}

void NexusSettingsHandler::HandleRefreshProviderModels(
    const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 2u);
  const std::string& callback_id = args[0].GetString();
  const std::string& provider_id = args[1].GetString();

  nexus::LlmBroker* broker = GetBroker();
  if (!broker) {
    RejectJavascriptCallback(
        base::Value(callback_id),
        base::Value("Nexus LLM Broker is not available"));
    return;
  }

  if (!broker->GetProvider(provider_id)) {
    base::DictValue result;
    result.Set("success", false);
    result.Set("error", "Unknown provider: " + provider_id);
    ResolveJavascriptCallback(base::Value(callback_id), std::move(result));
    return;
  }

  // OAuth providers need a backend-ready token applied before discovery. The
  // stored api_key may not be directly usable (github-copilot stores a GitHub
  // OAuth token that must be exchanged for a Copilot session token) or may be
  // near expiry (chatgpt-pro). EnsureFreshToken handles both; the result is
  // applied to the provider in OnOAuthTokenForModels before fetching /models.
  if (nexus::IsOAuthProvider(provider_id) && broker->oauth_manager()) {
    broker->oauth_manager()->EnsureFreshToken(
        provider_id,
        base::BindOnce(&NexusSettingsHandler::OnOAuthTokenForModels,
                       weak_factory_.GetWeakPtr(), callback_id, provider_id));
    return;
  }

  broker->RefreshModels(
      provider_id,
      base::BindOnce(&NexusSettingsHandler::OnProviderModelsFetched,
                     weak_factory_.GetWeakPtr(), callback_id));
}

void NexusSettingsHandler::OnOAuthTokenForModels(const std::string& callback_id,
                                                 const std::string& provider_id,
                                                 bool ok,
                                                 std::string access_token,
                                                 std::string account_id) {
  nexus::LlmBroker* broker = GetBroker();
  if (!broker) {
    RejectJavascriptCallback(
        base::Value(callback_id),
        base::Value("Nexus LLM Broker is not available"));
    return;
  }

  if (!ok || access_token.empty()) {
    base::DictValue result;
    result.Set("success", false);
    result.Set("error",
               "Could not obtain a fresh access token. Please sign in again.");
    ResolveJavascriptCallback(base::Value(callback_id), std::move(result));
    return;
  }

  // Apply the fresh, backend-ready token in-memory (the token store already
  // persisted the durable credential; a partial Configure dict here would
  // clobber default_model et al. if written to prefs).
  if (nexus::LlmProvider* provider = broker->GetProvider(provider_id)) {
    base::DictValue config;
    config.Set("api_key", access_token);
    config.Set("account_id", account_id);
    provider->Configure(config);
  }

  broker->RefreshModels(
      provider_id,
      base::BindOnce(&NexusSettingsHandler::OnProviderModelsFetched,
                     weak_factory_.GetWeakPtr(), callback_id));
}

void NexusSettingsHandler::HandleSetProviderApiKey(
    const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 3u);
  const std::string& callback_id = args[0].GetString();
  const std::string& provider_id = args[1].GetString();
  const std::string& api_key = args[2].GetString();

  Profile* profile = GetProfile();
  nexus::LlmBroker* broker = GetBroker();
  if (!profile || !broker) {
    RejectJavascriptCallback(
        base::Value(callback_id),
        base::Value("Nexus LLM Broker is not available"));
    return;
  }

  nexus::LlmProvider* provider = broker->GetProvider(provider_id);
  if (!provider) {
    base::DictValue result;
    result.Set("success", false);
    result.Set("error", "Unknown provider: " + provider_id);
    ResolveJavascriptCallback(base::Value(callback_id), std::move(result));
    return;
  }

  base::DictValue updates;
  updates.Set("api_key", api_key);

  PrefService* prefs = profile->GetPrefs();
  base::DictValue merged_config =
      MergeProviderConfigUpdates(prefs, provider, provider_id, updates);
  bool success = broker->ConfigureProvider(provider_id, merged_config);

  base::DictValue result;
  result.Set("success", success);
  result.Set("isConfigured", provider->IsConfigured());
  if (!success) {
    result.Set("error", "Failed to configure provider: " + provider_id);
  }

  ResolveJavascriptCallback(base::Value(callback_id),
                            std::move(result));
}

void NexusSettingsHandler::HandleValidateProviderConnection(
    const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 2u);
  const std::string& callback_id = args[0].GetString();
  const std::string& provider_id = args[1].GetString();

  Profile* profile = GetProfile();
  nexus::LlmBroker* broker = GetBroker();
  if (!profile || !broker) {
    RejectJavascriptCallback(
        base::Value(callback_id),
        base::Value("Nexus LLM Broker is not available"));
    return;
  }

  nexus::LlmProvider* provider = broker->GetProvider(provider_id);
  if (!provider) {
    base::DictValue result;
    result.Set("success", false);
    result.Set("error", "Unknown provider: " + provider_id);
    ResolveJavascriptCallback(base::Value(callback_id),
                              std::move(result));
    return;
  }

  if (!provider->IsConfigured()) {
    base::DictValue result;
    result.Set("success", false);
    result.Set("error",
               "Provider is not configured. Please review the provider "
               "settings and try again.");
    ResolveJavascriptCallback(base::Value(callback_id),
                              std::move(result));
    return;
  }

  const std::string model_name =
      GetConfiguredModelName(profile->GetPrefs(), provider_id, provider);
  base::DictValue call_options = BuildValidationCallOptions(model_name);

  broker->DoGenerate(
      call_options,
      base::BindOnce(
          [](base::WeakPtr<NexusSettingsHandler> handler,
             std::string cb_id, std::string model,
             nexus::GenerateResult result,
             std::optional<std::string> error) {
            if (!handler) return;

            base::DictValue response;
            if (error.has_value()) {
              response.Set("success", false);
              response.Set("error", error.value());
            } else {
              response.Set("success", true);
              response.Set("modelName", model);
            }
            handler->ResolveJavascriptCallback(
                base::Value(cb_id),
                std::move(response));
          },
          weak_factory_.GetWeakPtr(), callback_id, model_name));
}

void NexusSettingsHandler::HandleGetOllamaModels(
    const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 2u);
  const std::string& callback_id = args[0].GetString();
  const std::string& base_url = args[1].GetString();

  Profile* profile = GetProfile();
  nexus::LlmBroker* broker = GetBroker();
  if (!profile || !broker) {
    RejectJavascriptCallback(
        base::Value(callback_id),
        base::Value("Nexus LLM Broker is not available"));
    return;
  }

  nexus::LlmProvider* provider = broker->GetProvider("ollama");
  if (!provider) {
    base::DictValue result;
    result.Set("success", false);
    result.Set("error", "Unknown provider: ollama");
    ResolveJavascriptCallback(base::Value(callback_id), std::move(result));
    return;
  }

  base::DictValue updates;
  updates.Set("base_url", base_url);

  PrefService* prefs = profile->GetPrefs();
  base::DictValue merged_config =
      MergeProviderConfigUpdates(prefs, provider, "ollama", updates);
  broker->ConfigureProvider("ollama", merged_config);

  broker->RefreshModels(
      "ollama",
      base::BindOnce(&NexusSettingsHandler::OnOllamaModelsFetched,
                     weak_factory_.GetWeakPtr(), callback_id));
}

void NexusSettingsHandler::OnOllamaModelsFetched(
    const std::string& callback_id,
    std::vector<nexus::ModelInfo> models) {
  base::DictValue result;
  result.Set("success", true);
  result.Set("models", BuildModelList(std::move(models)));

  ResolveJavascriptCallback(base::Value(callback_id),
                            std::move(result));
}

void NexusSettingsHandler::OnProviderModelsFetched(
    const std::string& callback_id,
    std::vector<nexus::ModelInfo> models) {
  base::DictValue result;
  result.Set("success", true);
  result.Set("models", BuildModelList(std::move(models)));

  ResolveJavascriptCallback(base::Value(callback_id),
                            std::move(result));

  // The fetch above ran through LlmBroker::RefreshModels, which persists the
  // discovered list into the provider's in-memory `fetched_models_` (read by
  // GetSupportedModels()). Re-broadcast the enriched catalog so the always-
  // mounted topbar quick-switch popup rebuilds with the freshly discovered
  // models. Without this the popup keeps the stale catalog it captured at
  // boot — the exact symptom for OAuth providers (chatgpt-pro) whose models
  // are discovered live the first time their settings card opens.
  BroadcastEnrichedCatalog_();
}

void NexusSettingsHandler::OnVerifyProviderModelsFetched(
    const std::string& callback_id,
    std::vector<nexus::ModelInfo> models) {
  auto it = pending_verifications_.find(callback_id);
  if (it == pending_verifications_.end()) {
    return;
  }

  PendingVerification pending = std::move(it->second);
  pending_verifications_.erase(it);

  if (models.empty()) {
    ResolveJavascriptCallback(
        base::Value(callback_id),
        BuildVerificationFailure("Connected, but no models were returned.",
                                 kErrorCodeDiscoveryNoModels));
    return;
  }

  base::DictValue result;
  result.Set("success", true);
  result.Set("providerId", pending.provider_id);
  result.Set("baseUrl", pending.base_url);
  result.Set(
      "verifiedAtMs",
      static_cast<double>(base::Time::Now().InMillisecondsSinceUnixEpoch()));
  result.Set("modelCount", static_cast<int>(models.size()));
  result.Set("models", BuildModelList(std::move(models)));
  ResolveJavascriptCallback(base::Value(callback_id), std::move(result));
}

void NexusSettingsHandler::HandleSetActiveProvider(
    const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 2u);
  const std::string& callback_id = args[0].GetString();
  const std::string& provider_id = args[1].GetString();

  nexus::LlmBroker* broker = GetBroker();
  if (!broker) {
    RejectJavascriptCallback(
        base::Value(callback_id),
        base::Value("Nexus LLM Broker is not available"));
    return;
  }

  bool success = broker->SetActiveProvider(provider_id);

  base::DictValue result;
  result.Set("success", success);
  if (!success) {
    result.Set("error", "Unknown provider: " + provider_id);
  }

  ResolveJavascriptCallback(base::Value(callback_id),
                            std::move(result));
  if (success) {
    BroadcastEnrichedCatalog_();
  }
}

void NexusSettingsHandler::HandleBeginProviderOAuth(
    const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 2u);
  const std::string& callback_id = args[0].GetString();
  const std::string& provider_id = args[1].GetString();

  Profile* profile = GetProfile();
  nexus::LlmBroker* broker = GetBroker();
  if (!profile || !broker || !broker->oauth_manager()) {
    RejectJavascriptCallback(base::Value(callback_id),
                             base::Value("OAuth is not available"));
    return;
  }

  // Device-code providers (GitHub Copilot, Qwen Code) don't use a redirect:
  // request a device code and return the user code + verification URL for the
  // UI to display, then poll in the background.
  const nexus::OAuthProviderConfig* oauth_config =
      nexus::GetOAuthProviderConfig(provider_id);
  if (oauth_config && oauth_config->flow == nexus::OAuthFlow::kDeviceCode) {
    broker->oauth_manager()->StartDeviceCodeFlow(
        provider_id,
        base::BindOnce(&NexusSettingsHandler::OnDeviceCodeStarted,
                       weak_factory_.GetWeakPtr(), callback_id));
    return;
  }

  const std::string auth_url =
      broker->oauth_manager()->BeginLogin(provider_id);
  if (auth_url.empty()) {
    RejectJavascriptCallback(
        base::Value(callback_id),
        base::Value("Unknown OAuth provider: " + provider_id));
    return;
  }

  // Open the authorization page in a normal browser window (not the Nexus
  // pane) so the loopback redirect (http://localhost:1455/auth/callback) is
  // captured by the OAuthRedirectThrottle. Routed through OpenNexusAuthUrl —
  // a startup-registered hook backed by
  // custom_browser::window_util::OpenUrlInNormalWindow — because this handler
  // cannot depend on //chrome/browser/ui directly (it would form a GN cycle,
  // since //chrome/browser/ui already depends on the settings handler).
  nexus::OpenNexusAuthUrl(profile, GURL(auth_url));

  base::DictValue result;
  result.Set("flow", "pkce");
  result.Set("started", true);
  ResolveJavascriptCallback(base::Value(callback_id), std::move(result));
}

void NexusSettingsHandler::OnDeviceCodeStarted(
    const std::string& callback_id,
    bool ok,
    nexus::NexusOAuthManager::DeviceCodeInfo info,
    std::string error) {
  if (!ok) {
    RejectJavascriptCallback(
        base::Value(callback_id),
        base::Value(error.empty() ? "Failed to start sign-in" : error));
    return;
  }

  // Open the verification page in a normal window so the user can authorize
  // without leaving the device code behind.
  if (Profile* profile = GetProfile();
      profile && !info.verification_uri.empty()) {
    nexus::OpenNexusAuthUrl(profile, GURL(info.verification_uri));
  }

  base::DictValue result;
  result.Set("flow", "device-code");
  result.Set("userCode", info.user_code);
  result.Set("verificationUri", info.verification_uri);
  result.Set("expiresIn", info.expires_in);
  ResolveJavascriptCallback(base::Value(callback_id), std::move(result));
}

void NexusSettingsHandler::HandleGetProviderOAuthStatus(
    const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 2u);
  const std::string& callback_id = args[0].GetString();
  const std::string& provider_id = args[1].GetString();

  base::DictValue result;
  nexus::LlmBroker* broker = GetBroker();
  if (broker && broker->oauth_manager()) {
    nexus::NexusOAuthManager* manager = broker->oauth_manager();
    result.Set("connected", manager->IsConnected(provider_id));
    const std::string email = manager->GetConnectedEmail(provider_id);
    if (!email.empty()) {
      result.Set("connectedEmail", email);
    }
  } else {
    result.Set("connected", false);
  }
  ResolveJavascriptCallback(base::Value(callback_id), std::move(result));
}

void NexusSettingsHandler::HandleGetWebappUrl(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 1u);
  const std::string& callback_id = args[0].GetString();

  Profile* profile = GetProfile();
  PrefService* prefs = profile ? profile->GetPrefs() : nullptr;
  const std::string value =
      prefs ? prefs->GetString(nexus::prefs::kNexusWebappUrl) : std::string();

  ResolveJavascriptCallback(base::Value(callback_id), base::Value(value));
}

void NexusSettingsHandler::HandleSetWebappUrl(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 2u);
  const std::string& callback_id = args[0].GetString();
  const std::string& value = args[1].GetString();

  Profile* profile = GetProfile();
  PrefService* prefs = profile ? profile->GetPrefs() : nullptr;
  if (!prefs) {
    RejectJavascriptCallback(base::Value(callback_id),
                             base::Value("Profile prefs unavailable"));
    return;
  }

  // Store the value verbatim (empty = use default). GetResolvedNexusGuestURL()
  // re-validates the stored URL at read time and falls back to the default if
  // it is not an allowed guest URL, so no scheme/host check is needed here.
  prefs->SetString(nexus::prefs::kNexusWebappUrl, value);
  ResolveJavascriptCallback(
      base::Value(callback_id),
      base::Value(prefs->GetString(nexus::prefs::kNexusWebappUrl)));
}

void NexusSettingsHandler::HandleGetBrowserToolsSeparateWindow(
    const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 1u);
  const std::string& callback_id = args[0].GetString();

  Profile* profile = GetProfile();
  PrefService* prefs = profile ? profile->GetPrefs() : nullptr;
  const bool enabled =
      prefs && prefs->GetBoolean(nexus::prefs::kNexusBrowserToolsSeparateWindow);

  ResolveJavascriptCallback(base::Value(callback_id), base::Value(enabled));
}

void NexusSettingsHandler::HandleSetBrowserToolsSeparateWindow(
    const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 2u);
  const std::string& callback_id = args[0].GetString();
  const bool enabled = args[1].GetBool();

  Profile* profile = GetProfile();
  PrefService* prefs = profile ? profile->GetPrefs() : nullptr;
  if (!prefs) {
    RejectJavascriptCallback(base::Value(callback_id),
                             base::Value("Profile prefs unavailable"));
    return;
  }

  prefs->SetBoolean(nexus::prefs::kNexusBrowserToolsSeparateWindow, enabled);
  ResolveJavascriptCallback(
      base::Value(callback_id),
      base::Value(
          prefs->GetBoolean(nexus::prefs::kNexusBrowserToolsSeparateWindow)));
}

void NexusSettingsHandler::HandleGetAgentConfig(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 1u);
  const std::string& callback_id = args[0].GetString();

  // Returns the effective config (built-in defaults overlaid with stored
  // overrides). The UI reads keys it knows (maxTurns, stepTimeoutSec, …).
  ResolveJavascriptCallback(
      base::Value(callback_id),
      base::Value(nexus::GetAgentConfig(GetProfile())));
}

void NexusSettingsHandler::HandleSetAgentConfig(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 3u);
  const std::string& callback_id = args[0].GetString();
  const std::string& key = args[1].GetString();
  // args[2] is the value — number (maxTurns/stepTimeoutSec) or dict (model).
  // Stored verbatim; per-key validation/clamping lives in the UI (UX) and the
  // consumer (engine.applyRuntimeSettings, the safety backstop).

  Profile* profile = GetProfile();
  PrefService* prefs = profile ? profile->GetPrefs() : nullptr;
  if (!prefs) {
    RejectJavascriptCallback(base::Value(callback_id),
                             base::Value("Profile prefs unavailable"));
    return;
  }

  {
    ScopedDictPrefUpdate update(prefs, nexus::prefs::kNexusAgentConfig);
    update->Set(key, args[2].Clone());
  }
  // Writing the pref fires the PrefChangeRegistrar in the session bridge
  // adapter, which pushes the full dict to the guest live. Echo the merged
  // config back for the WebUI to refresh its local view.
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(nexus::GetAgentConfig(profile)));
}

// --- Quick-settings popup handlers ------------------------------------------

base::DictValue NexusSettingsHandler::BuildEnrichedNexusCatalog_() {
  base::DictValue result;
  Profile* profile = GetProfile();
  nexus::LlmBroker* broker = GetBroker();
  if (!profile || !broker) {
    return result;
  }
  PrefService* prefs = profile->GetPrefs();

  // The wire-from-TS catalog is the source of truth for the model list.
  // Index it by providerId so we can splice each provider's curated
  // models into the enriched response below.
  const base::DictValue& ts_catalog = broker->GetTsModelCatalog();
  std::map<std::string, const base::DictValue*> ts_by_provider;
  if (const base::ListValue* providers = ts_catalog.FindList("providers")) {
    for (const base::Value& entry : *providers) {
      const base::DictValue* provider = entry.GetIfDict();
      if (!provider) continue;
      const std::string* id = provider->FindString("providerId");
      if (id) ts_by_provider[*id] = provider;
    }
  }

  base::ListValue providers;
  for (const nexus::ProviderInfo& info : broker->GetRegisteredProviders()) {
    nexus::LlmProvider* provider = broker->GetProvider(info.id);
    // OAuth providers: "signed in" (tokens present) is the source of truth for
    // configured/verified, since the in-memory api_key / prefs last_verification
    // can lag and would otherwise hide a working provider from the quick-switch.
    const bool oauth_connected =
        nexus::IsOAuthProvider(info.id) && broker->oauth_manager() &&
        broker->oauth_manager()->IsConnected(info.id);
    const bool is_configured =
        (provider && provider->IsConfigured()) || oauth_connected;
    const bool is_verified =
        is_configured &&
        (oauth_connected || nexus::IsNexusProviderVerified(prefs, info.id));

    base::DictValue provider_dict;
    provider_dict.Set("providerId", info.id);
    provider_dict.Set("label", info.name);
    provider_dict.Set("iconSvg", nexus::GetNexusProviderIconSvg(info.id));
    provider_dict.Set("isConfigured", is_configured);
    provider_dict.Set("isVerified", is_verified);

    // Pass through the user's enabled-models allowlist so the popup can
    // hide models the user explicitly turned off in Settings.
    base::DictValue stored_config = GetStoredProviderConfigCopy(prefs, info.id);
    if (const base::ListValue* enabled =
            stored_config.FindList("enabled_models")) {
      provider_dict.Set("enabledModels", enabled->Clone());
    }

    // Splice the model list (+ any filterMode) from the TS-pushed catalog.
    // OAuth providers (chatgpt-pro, github-copilot, qwen-code) have no TS
    // config — their models are discovered live — so when the TS catalog has
    // none, fall back to the provider's fetched model list. Without this the
    // topbar quick-switch popup shows no models for those providers even after
    // discovery populated them.
    base::ListValue model_list;
    bool have_models = false;
    auto it = ts_by_provider.find(info.id);
    if (it != ts_by_provider.end() && it->second) {
      if (const std::string* filter_mode =
              it->second->FindString("filterMode")) {
        provider_dict.Set("filterMode", *filter_mode);
      }
      if (const base::ListValue* models = it->second->FindList("models");
          models && !models->empty()) {
        model_list = models->Clone();
        have_models = true;
      }
    }
    if (!have_models) {
      for (const nexus::ModelInfo& model : info.models) {
        base::DictValue model_dict;
        model_dict.Set("model", model.id);
        model_dict.Set("name", model.name.empty() ? model.id : model.name);
        if (model.context_window > 0) {
          model_dict.Set("contextWindow", model.context_window);
        }
        if (model.default_thinking_tier &&
            !model.default_thinking_tier->empty()) {
          model_dict.Set("defaultThinkingTier", *model.default_thinking_tier);
        }
        if (!model.available_thinking_tiers.empty()) {
          base::ListValue tiers;
          for (const std::string& tier : model.available_thinking_tiers) {
            tiers.Append(tier);
          }
          model_dict.Set("availableThinkingTiers", std::move(tiers));
        }
        model_list.Append(std::move(model_dict));
      }
    }
    provider_dict.Set("models", std::move(model_list));

    providers.Append(std::move(provider_dict));
  }

  result.Set("providers", std::move(providers));
  result.Set("activeProviderId", broker->GetActiveProviderId());
  return result;
}

void NexusSettingsHandler::BroadcastEnrichedCatalog_() {
  if (!IsJavascriptAllowed()) return;
  FireWebUIListener("nexusCatalogChanged",
                    base::Value(BuildEnrichedNexusCatalog_()));
}

void NexusSettingsHandler::HandleGetNexusCatalog(const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 1u);
  const std::string& callback_id = args[0].GetString();
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(BuildEnrichedNexusCatalog_()));
}

void NexusSettingsHandler::HandleGetNexusSessionStatus(
    const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 1u);
  const std::string& callback_id = args[0].GetString();

  nexus::LlmBroker* broker = GetBroker();
  if (!broker) {
    ResolveJavascriptCallback(base::Value(callback_id),
                              base::Value(base::DictValue()));
    return;
  }
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(broker->GetSessionStatus().Clone()));
}

void NexusSettingsHandler::HandlePickNexusSelection(
    const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 2u);
  const std::string& callback_id = args[0].GetString();
  const base::Value& selection_value = args[1];
  if (!selection_value.is_dict()) {
    RejectJavascriptCallback(
        base::Value(callback_id),
        base::Value("pickNexusSelection expected a {providerId,modelId,…} dict"));
    return;
  }
  const base::DictValue& selection = selection_value.GetDict();

  const std::string* provider_id = selection.FindString("providerId");
  const std::string* model_id = selection.FindString("modelId");
  if (!provider_id || provider_id->empty() || !model_id || model_id->empty()) {
    RejectJavascriptCallback(
        base::Value(callback_id),
        base::Value("pickNexusSelection requires non-empty providerId+modelId"));
    return;
  }
  std::string thinking_tier;
  if (const std::string* tier = selection.FindString("thinkingTier")) {
    thinking_tier = *tier;
  }

  nexus::LlmBroker* broker = GetBroker();
  if (!broker) {
    RejectJavascriptCallback(
        base::Value(callback_id),
        base::Value("Nexus LLM Broker is not available"));
    return;
  }

  nexus::LlmBroker::DispatchPickResult dispatch =
      broker->DispatchUserPick(*provider_id, *model_id, thinking_tier);

  base::DictValue result;
  result.Set("ok", dispatch.ok);
  if (!dispatch.ok) {
    result.Set("errorCode", dispatch.error_code);
    result.Set("error", dispatch.error_message);
    // Mirror the failure to anyone listening for live updates so a popup
    // anchor that's already showing a pending state can recover.
    if (IsJavascriptAllowed()) {
      base::DictValue payload;
      payload.Set("providerId", *provider_id);
      payload.Set("modelId", *model_id);
      payload.Set("errorCode", dispatch.error_code);
      payload.Set("error", dispatch.error_message);
      FireWebUIListener("nexusPickFailed", base::Value(std::move(payload)));
    }
  } else {
    result.Set("requestId", dispatch.request_id);
    // Echo whether the bridge-event pusher was registered + invoked.
    // Lets the WebUI / e2e harness distinguish "C++ accepted but
    // nexus_web isn't subscribed yet" from "C++ accepted and event
    // pushed". When false, the popup pick is durable in prefs but the
    // active-session rebind is not happening (guest webview not ready
    // or its bridge port isn't alive).
    result.Set("pusherInvoked", dispatch.pusher_invoked);
  }
  ResolveJavascriptCallback(base::Value(callback_id), std::move(result));
}

}  // namespace settings
