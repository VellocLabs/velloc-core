// Copyright 2024 The Custom Browser Authors
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
#include "custom_browser/browser/nexus/nexus_secret_crypt.h"
#include "custom_browser/browser/nexus/api/llm/provider_config.h"
#include "custom_browser/browser/nexus/api/mcp/nexus_mcp_host.h"
#include "custom_browser/browser/nexus/api/mcp/nexus_mcp_server_store.h"
#include "custom_browser/browser/nexus/launcher/nexus_launcher_prefs.h"
#include "custom_browser/browser/nexus/nexus_browser_tools_config.h"
#include "custom_browser/browser/nexus/nexus_client_restart.h"
#include "custom_browser/browser/nexus/tools/process_tool/nexus_shell_service_launcher.h"
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
                                           nexus::LlmProvider* provider,
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
  // `api/providers/nexus_providers_bridge_adapter.cc` for the new path
  // and the parity-audit doc for the migration record.
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
  // `getAgentConfig` / `setAgentConfig` were the WebUI surface for
  // the `nexus.agent_config` Profile pref before the about-page was
  // migrated to `~/.nexus/settings.json` (under `agent`). The pref
  // itself still exists — `nexus_guest_config.cc`,
  // `nexus_session_bridge_adapter`, and `llm_broker.cc` read it for
  // boot-time guest URL flags + live runtime push + default model
  // resolution — but no JS code calls the sendWithPromise routes
  // anymore, so the registrations were dropped to keep the WebUI
  // surface honest about what's actually consumed. Re-introducing
  // these routes is the wrong fix if a future surface wants live
  // agent-config: subscribe to the new `agent.changed` bridge event
  // instead.
  //
  // Quick-settings popup support — read-side handlers + the
  // user-pick write-side handler. See nexus_settings_handler.h for
  // full contracts.
  web_ui()->RegisterMessageCallback(
      "getNexusCatalog",
      base::BindRepeating(&NexusSettingsHandler::HandleGetNexusCatalog,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "getNexusSessionStatus",
      base::BindRepeating(&NexusSettingsHandler::HandleGetNexusSessionStatus,
                          base::Unretained(this)));
  // `pickNexusSelection` route removed: migrated to `providers.setActive`
  // on the panel WebUI bridge port (A5 Tier 3b). See
  // api/providers/nexus_providers_bridge_adapter.cc::HandleSetActiveOnUI.
  // MCP server configuration — only the connection-probe handler
  // survives. `getMcpServers` / `upsertMcpServer` / `removeMcpServer`
  // were dropped when the MCP settings page moved to the WebUI ↔
  // SharedWorker bridge (servers persist to `~/.nexus/settings.json`
  // under `mcp.servers` now). The C++ `nexus.mcp_server_configs`
  // pref is still registered for back-compat with the bridge
  // adapter's `GetOrCreateHost` legacy fallback path, but no JS
  // code reads or writes it through the WebUI surface anymore.
  // A5 Tier 3a: `testMcpServer` chrome.send route migrated to
  // `mcp.testConnect` on the panel WebUI bridge port. Legacy handler
  // body in this file is dead code while diagnostic logging is
  // running on the new path; will retire in a follow-up.
}

void NexusSettingsHandler::OnJavascriptAllowed() {
  // Subscribe to broker change signals so we can fan them out to the
  // Lit `<nexus-quick-settings>` popup. The observation only fires while
  // JS is allowed; if JS gets disabled (page reload mid-session) we
  // tear down in OnJavascriptDisallowed.
  if (nexus::LlmBroker* broker = GetBroker()) {
    broker_observation_.Observe(broker);
  }
  // Prime the at-rest secret encryptor so api_key / MCP-secret writes from
  // this settings page store ciphertext rather than degrading to plaintext.
  nexus::NexusSecretCrypt::GetInstance()->EnsureAcquired(
      g_browser_process ? g_browser_process->os_crypt_async() : nullptr);
}

void NexusSettingsHandler::OnJavascriptDisallowed() {
  broker_observation_.Reset();
  pending_verifications_.clear();
  // `pending_mcp_tests_.clear()` removed: the transient-host map moved
  // to NexusMcpBridgeAdapter (A5 Tier 3a) which owns its own teardown.
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

// HandleGetNexusViewState removed — registered + impl'd but never
// called from JS. Was a leftover wrapper around
// `broker->GetActiveProviderId()` from an earlier UI iteration; the
// quick-settings popup reads the active provider from
// `getNexusCatalog`'s response instead.

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

// HandleGetMcpServers / HandleUpsertMcpServer / HandleRemoveMcpServer
// were the WebUI CRUD surface for the `nexus.mcp_server_configs`
// Profile pref before the MCP settings page moved to the WebUI ↔
// SharedWorker bridge. They've been dropped — no JS code calls them
// anymore. The pref itself + `NexusMcpServerStore` remain, because
// `NexusMcpBridgeAdapter`'s legacy `GetOrCreateHost(id)` fallback
// still consults it for hosts that haven't received an inline
// config from the new path.

// HandleTestMcpServer + OnTestMcpServerConnected removed: migrated to
// `mcp.testConnect` on the panel WebUI bridge port (A5 Tier 3a). See
// api/mcp/nexus_mcp_bridge_adapter.cc::HandleTestConnectOnUI.

// HandleRefreshProviderModels + OnOAuthTokenForModels +
// OnProviderModelsFetched removed — the `refreshProviderModels`
// chrome.send route is fully migrated to the panel ↔ C++ bridge
// (`providers.refresh`). See
// `api/providers/nexus_providers_bridge_adapter.cc::HandleRefreshOnUI`
// for the replacement (and its `OnOAuthTokenForRefresh` /
// `OnRefreshModelsFetched` continuations).
//
// Behavioural delta vs. the legacy path: the new adapter does NOT
// re-fire `nexusCatalogChanged` after the refresh resolves. The legacy
// path did so via `BroadcastEnrichedCatalog_` to refresh the
// always-mounted topbar quick-switch popup. The quick-switch popup
// will pick up newly-discovered models on its next catalog read (e.g.
// reopen, or any other `nexusCatalogChanged` trigger like
// `HandleUpdateProviderConfig` / `HandleResetProviderConfig`). The
// followup is to move `BroadcastEnrichedCatalog_` itself onto the
// broker so any caller — chrome.send handler or bridge adapter — can
// fire it uniformly.
//
// HandleSetProviderApiKey, HandleValidateProviderConnection,
// HandleGetOllamaModels + OnOllamaModelsFetched removed earlier — all
// four were vapor (registered + implemented but never called from
// JS). `HandleSetProviderApiKey` was a single-field shortcut over the
// unified `HandleUpdateProviderConfig` flow.
// `HandleValidateProviderConnection` was a synthesized "try a tiny
// generate" connection test, replaced by the real
// `HandleVerifyProviderConfig` path. `HandleGetOllamaModels` was an
// ollama-special-case shortcut over `HandleRefreshProviderModels`.

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

// HandleSetActiveProvider removed — vapor. The active provider is
// pinned via the unified pick flow (`HandlePickNexusSelection`),
// which calls `broker->DispatchUserPick` and ALSO writes
// `nexus.active_provider_id` indirectly through
// `LlmBroker::SetDefaultSelection`. This standalone setter had no
// callers in the WebUI.

// HandleBeginProviderOAuth + OnDeviceCodeStarted +
// HandleGetProviderOAuthStatus removed — `beginProviderOAuth` and
// `getProviderOAuthStatus` chrome.send routes are fully migrated to
// the panel ↔ C++ bridge (`providers.beginOAuth` /
// `providers.oauthStatus`). See
// `api/providers/nexus_providers_bridge_adapter.cc::HandleBeginOAuthOnUI`
// and `::HandleOauthStatusOnUI` for the replacements. Both keep the
// same device-code vs. PKCE branching, the same OpenNexusAuthUrl
// window-opening behaviour, and the same `{connected, connectedEmail?}`
// status shape.

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
  // (2026-09-10): prefs::kBackgroundModeEnabled drives the keep-alive + tray
  // icon (NexusBackgroundModeManager) and the background panel.
  HandleGetBackgroundModeEnabled(args);
}

void NexusSettingsHandler::HandleSetNexusLauncherEnabled(
    const base::ListValue& args) {
  // Alias of setBackgroundModeEnabled (see HandleGetNexusLauncherEnabled).
  HandleSetBackgroundModeEnabled(args);
}

// HandleGetAgentConfig + HandleSetAgentConfig were the WebUI surface
// for the `nexus.agent_config` Profile pref before the about page
// moved to `~/.nexus/settings.json` (`agent` section, served via the
// WebUI ↔ SharedWorker bridge). Dropped: no JS caller. The pref
// itself stays for now — `llm_broker.cc` still writes
// `nexus.agent_config.model` on every active-selection change and the
// session-bridge live-push observer fans that out as a
// `session.applyAgentConfig` event the guest model picker
// subscribes to. (Path 1 retired the `nexus_guest_config.cc` URL flag
// mapping in the same series; Path 2 wired the `agent.changed`
// JS-side broadcast for log/trace/maxTurns/stepTimeoutSec. A future
// PR can move `model` into settings.json too and finally retire the
// C++ observer.)

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
    bool passthrough = false;
    auto it = ts_by_provider.find(info.id);
    if (it != ts_by_provider.end() && it->second) {
      if (const std::string* filter_mode =
              it->second->FindString("filterMode")) {
        provider_dict.Set("filterMode", *filter_mode);
        passthrough = (*filter_mode == "passthrough");
      }
      // For a `curated` provider the TS list IS the catalog. For a
      // `passthrough` provider (e.g. chatgpt-pro, whose models are discovered
      // live from the Codex /models endpoint) the TS `models` are only a
      // guest-side config OVERLAY — token limits / display names applied in
      // `createPassthroughProvider` — and must NOT shrink the popup's list.
      // So skip adopting the TS list when passthrough, and fall through to the
      // discovered `info.models` below so the popup still shows every model.
      if (!passthrough) {
        if (const base::ListValue* models = it->second->FindList("models");
            models && !models->empty()) {
          model_list = models->Clone();
          have_models = true;
        }
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

// HandlePickNexusSelection removed: migrated to `providers.setActive` on
// the panel WebUI bridge port (A5 Tier 3b). See
// api/providers/nexus_providers_bridge_adapter.cc::HandleSetActiveOnUI.
// The new adapter does the exact same DispatchUserPick + response
// shape, with the InvalidArgs path replacing the legacy reject.

}  // namespace settings
