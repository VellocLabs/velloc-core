// Copyright 2024 VellocLabs
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/settings/nexus_settings_handler.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/buildflags.h"
#include "chrome/common/pref_names.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/reload_type.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "custom_browser/browser/nexus/api/llm/llm_broker.h"
#include "custom_browser/browser/nexus/api/llm/llm_broker_factory.h"
#include "custom_browser/browser/nexus/api/llm/llm_pref_names.h"
#include "custom_browser/browser/nexus/api/llm/llm_provider.h"
#include "custom_browser/browser/nexus/api/llm/llm_provider_factory.h"
#include "custom_browser/browser/nexus/api/llm/llm_types.h"
#include "custom_browser/browser/nexus/api/llm/oauth/oauth_manager.h"
#include "custom_browser/browser/nexus/api/llm/oauth/oauth_providers.h"
#include "custom_browser/browser/nexus/api/llm/oauth/oauth_url_opener.h"
#include "custom_browser/browser/nexus/nexus_secret_crypt.h"
#include "custom_browser/browser/nexus/api/llm/provider_config.h"
#include "custom_browser/browser/nexus/launcher/nexus_launcher_prefs.h"
#include "custom_browser/browser/nexus/nexus_browser_tools_config.h"
#include "custom_browser/browser/nexus/nexus_client_restart.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
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

// EncryptSecretDictValues / DecryptSecretDictValues were used by the
// MCP CRUD handlers (now dropped) to round-trip `env` / `headers`
// values through `NexusSecretCrypt`. Removed alongside the handlers
// — `NexusSecretCrypt` itself stays on the C++ side for any future
// secrets-at-rest path; helpers can be reintroduced inline at the
// new caller if/when one shows up.

base::DictValue GetStoredProviderConfigCopy(PrefService* prefs,
                                            const std::string& provider_id) {
  const base::DictValue* config =
      nexus::GetStoredProviderConfig(prefs, provider_id);
  return config ? config->Clone() : base::DictValue();
}

base::DictValue MergeProviderConfigUpdates(PrefService* prefs,
                                           const std::string& provider_id,
                                           const base::DictValue& updates) {
  base::DictValue merged = GetStoredProviderConfigCopy(prefs, provider_id);

  if (const auto* api_key = updates.FindString("api_key");
      api_key && !api_key->empty()) {
    // Encrypt the api_key at rest; the provider's Configure() decrypts on
    // read. (A re-saved plaintext key is migrated to ciphertext here.)
    merged.Set("api_key",
               nexus::NexusSecretCrypt::GetInstance()->Encrypt(*api_key));
  }

  if (const auto* account_token = updates.FindString("account_token");
      account_token && !account_token->empty()) {
    // Encrypt the account token at rest (idempotent — a re-saved already-
    // encrypted value is left as-is). Any future reader decrypts via
    // NexusSecretCrypt::Decrypt, which passes legacy plaintext through.
    merged.Set("account_token",
               nexus::NexusSecretCrypt::GetInstance()->Encrypt(*account_token));
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

  // Preset User-Agent — same rule as the `providerAuth` adapter's copy:
  // present ('' = host default) replaces the stored one, absent keeps it.
  if (updates.Find("user_agent")) {
    nexus::SetStoredUserAgent(updates.FindString("user_agent"), merged);
  }

  // An empty base_url means "use the default" and is stored as '' — never
  // as the default's current value, which would pin the provider to it after
  // the guest-owned default moves (Configure() resolves '' to the default).
  if (updates.Find("base_url")) {
    const std::string* base_url = updates.FindString("base_url");
    merged.Set("base_url", base_url ? *base_url : std::string());
  }

  return merged;
}

base::DictValue BuildVerificationConfig(PrefService* prefs,
                                        const std::string& provider_id,
                                        const base::DictValue& updates) {
  base::DictValue merged =
      MergeProviderConfigUpdates(prefs, provider_id, updates);
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

// BuildValidationCallOptions removed — its only caller
// (HandleValidateProviderConnection) was vapor and was deleted.

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
  // `getNexusViewState`, `setProviderApiKey`, `validateProviderConnection`,
  // `getOllamaModels`, `setActiveProvider` were vapor — registered + impl'd
  // but never called from JS. Predated the unified `updateProviderConfig`
  // flow. Dropped end-to-end (registration here, Handle* impls below, .h
  // declarations).
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
  // refreshProviderModels / beginProviderOAuth / getProviderOAuthStatus
  // registrations removed — all three routes migrated to the panel ↔ C++
  // bridge (`providers.refresh` / `providers.beginOAuth` /
  // `providers.oauthStatus`). Zero remaining chrome.send callers
  // anywhere in the codebase (verified via string-literal grep). See
  // `api/providers/nexus_providers_bridge_adapter.cc` for the new path.
  web_ui()->RegisterMessageCallback(
      "getWebappUrl",
      base::BindRepeating(&NexusSettingsHandler::HandleGetWebappUrl,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "setWebappUrl",
      base::BindRepeating(&NexusSettingsHandler::HandleSetWebappUrl,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "restartBrowser",
      base::BindRepeating(&NexusSettingsHandler::HandleRestartBrowser,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "reloadPanel",
      base::BindRepeating(&NexusSettingsHandler::HandleReloadPanel,
                          base::Unretained(this)));
  // `getBrowserToolsSeparateWindow` / `setBrowserToolsSeparateWindow`
  // routes removed: migrated to `browserTools.snapshot` /
  // `browserTools.setField` on the panel WebUI bridge port (A5 Tier 2).
  // See api/browserTools/nexus_browser_tools_bridge_adapter.cc.
  // `getBrowserToolsAllowEvaluate` / `setBrowserToolsAllowEvaluate`
  // routes removed alongside the Settings · Tools opt-in toggle.
  // BrowserEvaluate is now always available — no gate, no pref. See
  // nexus_browser_tools_config.h.
  web_ui()->RegisterMessageCallback(
      "getBackgroundModeEnabled",
      base::BindRepeating(
          &NexusSettingsHandler::HandleGetBackgroundModeEnabled,
          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "setBackgroundModeEnabled",
      base::BindRepeating(
          &NexusSettingsHandler::HandleSetBackgroundModeEnabled,
          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "getNexusLauncherEnabled",
      base::BindRepeating(
          &NexusSettingsHandler::HandleGetNexusLauncherEnabled,
          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "setNexusLauncherEnabled",
      base::BindRepeating(
          &NexusSettingsHandler::HandleSetNexusLauncherEnabled,
          base::Unretained(this)));
}

void NexusSettingsHandler::OnJavascriptAllowed() {
  // Prime the at-rest secret encryptor so api_key / MCP-secret writes from
  // this settings page store ciphertext rather than degrading to plaintext.
  nexus::NexusSecretCrypt::GetInstance()->EnsureAcquired(
      g_browser_process ? g_browser_process->os_crypt_async() : nullptr);
}

void NexusSettingsHandler::OnJavascriptDisallowed() {
  pending_verifications_.clear();
  weak_factory_.InvalidateWeakPtrs();
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

// HandleGetNexusViewState removed — registered + impl'd but never
// called from JS. Was a leftover wrapper around
// `broker->GetActiveProviderId()` from an earlier UI iteration.

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
      MergeProviderConfigUpdates(prefs, provider_id, *updates);
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

  // A factory-fresh provider has no default base URL — the guest-owned one
  // lives on the persisted instance, applied from the published catalog.
  transient_provider->SetDefaultBaseUrl(
      persisted_provider->GetDefaultBaseUrl());
  base::DictValue verification_config =
      BuildVerificationConfig(profile->GetPrefs(), provider_id, *updates);
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
  // next. The host has no inference path (the runtime speaks every wire
  // format), so a model listing is the only probe it can make.
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

// HandleSetActiveProvider removed — vapor. This standalone setter had no
// callers in the WebUI; the active provider is pinned through the panel
// bridge (`providers.setActive`) instead.


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

void NexusSettingsHandler::HandleRestartBrowser(
    const base::ListValue& /*args*/) {
  // No AllowJavascript() and no callback: this route never answers. The
  // restart is posted (nexus::RestartClient), so this handler returns first
  // and WebUI teardown happens on the next task.
  nexus::RestartClient();
}

void NexusSettingsHandler::HandleReloadPanel(
    const base::ListValue& /*args*/) {
  // Posted: the reload replaces this WebUI (and this handler), so it must not
  // start while the message is still being dispatched to us.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<content::WebContents> contents) {
            if (contents) {
              contents->GetController().Reload(content::ReloadType::NORMAL,
                                               /*check_for_repost=*/false);
            }
          },
          web_ui()->GetWebContents()->GetWeakPtr()));
}

// HandleGetBrowserToolsSeparateWindow / HandleSetBrowserToolsSeparateWindow
// removed: migrated to the `browserTools.snapshot` / `browserTools.setField`
// bridge actions on the panel WebUI port (Tier 2 of the A5 migration). See
// api/browserTools/nexus_browser_tools_bridge_adapter.cc and the
// register-on-kPanel block in nexus_bridge_port_service.cc. The underlying
// pref (`nexus.browser_tools_separate_window`) and the runtime reader
// `ShouldHostAgentTabsInSeparateWindow` are unchanged.

// HandleGetBrowserToolsAllowEvaluate / HandleSetBrowserToolsAllowEvaluate
// removed alongside the Settings · Tools opt-in toggle. The pref name +
// registration are gone; BrowserEvaluate is now always available (no
// gate). See nexus_browser_tools_config.{h,cc}.

void NexusSettingsHandler::HandleGetBackgroundModeEnabled(
    const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 1u);
  const std::string& callback_id = args[0].GetString();

#if BUILDFLAG(ENABLE_BACKGROUND_MODE)
  // Browser-wide setting: BackgroundModeManager registers + observes this on
  // local_state, not on the profile prefs. Default is true.
  PrefService* local_state = g_browser_process->local_state();
  const bool enabled =
      local_state && local_state->GetBoolean(::prefs::kBackgroundModeEnabled);
#else
  const bool enabled = false;
#endif

  ResolveJavascriptCallback(base::Value(callback_id), base::Value(enabled));
}

void NexusSettingsHandler::HandleSetBackgroundModeEnabled(
    const base::ListValue& args) {
  AllowJavascript();
  CHECK_GE(args.size(), 2u);
  const std::string& callback_id = args[0].GetString();

#if BUILDFLAG(ENABLE_BACKGROUND_MODE)
  const bool enabled = args[1].GetBool();
  PrefService* local_state = g_browser_process->local_state();
  if (!local_state) {
    RejectJavascriptCallback(base::Value(callback_id),
                             base::Value("Local state unavailable"));
    return;
  }

  // Writing the pref is sufficient: BackgroundModeManager's PrefChangeRegistrar
  // on local_state reacts to it (OnBackgroundModeEnabledPrefChanged), exactly
  // as the chrome://settings/system toggle does.
  local_state->SetBoolean(::prefs::kBackgroundModeEnabled, enabled);
  ResolveJavascriptCallback(
      base::Value(callback_id),
      base::Value(local_state->GetBoolean(::prefs::kBackgroundModeEnabled)));
#else
  RejectJavascriptCallback(
      base::Value(callback_id),
      base::Value("Background mode is not supported on this platform"));
#endif
}

void NexusSettingsHandler::HandleGetNexusLauncherEnabled(
    const base::ListValue& args) {
  // Alias of getBackgroundModeEnabled since the two System toggles were merged
  // prefs::kBackgroundModeEnabled drives the keep-alive + tray
  // icon (NexusBackgroundModeManager) and the background panel.
  HandleGetBackgroundModeEnabled(args);
}

void NexusSettingsHandler::HandleSetNexusLauncherEnabled(
    const base::ListValue& args) {
  // Alias of setBackgroundModeEnabled (see HandleGetNexusLauncherEnabled).
  HandleSetBackgroundModeEnabled(args);
}


// HandlePickNexusSelection removed: migrated to `providers.setActive` on
// the panel WebUI bridge port (A5 Tier 3b). See
// api/providers/nexus_providers_bridge_adapter.cc::HandleSetActiveOnUI.
// The new adapter does the exact same DispatchUserPick + response
// shape, with the InvalidArgs path replacing the legacy reject.

}  // namespace settings
