#include "custom_browser/browser/nexus/api/providers/nexus_providers_bridge_adapter.h"

#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/storage_partition.h"
#include "custom_browser/browser/nexus/api/llm/llm_broker.h"
#include "custom_browser/browser/nexus/api/llm/llm_broker_factory.h"
#include "custom_browser/browser/nexus/api/llm/llm_provider.h"
#include "custom_browser/browser/nexus/api/llm/llm_provider_factory.h"
#include "custom_browser/browser/nexus/api/llm/llm_types.h"
#include "custom_browser/browser/nexus/api/llm/oauth/oauth_manager.h"
#include "custom_browser/browser/nexus/api/llm/oauth/oauth_providers.h"
#include "custom_browser/browser/nexus/api/llm/oauth/oauth_url_opener.h"
#include "custom_browser/browser/nexus/api/llm/llm_pref_names.h"
#include "custom_browser/browser/nexus/api/llm/provider_config.h"
#include "custom_browser/browser/nexus/nexus_secret_crypt.h"
#include "url/gurl.h"

namespace nexus {

namespace {

// Error-code constants — same wire strings the legacy
// `BuildVerificationFailure` uses, so WebUI clients see identical
// envelopes regardless of which transport delivered the failure.
constexpr char kErrorCodeAuthUnauthorized[] = "auth_unauthorized";
constexpr char kErrorCodeDiscoveryNoModels[] = "discovery_no_models";
constexpr char kErrorCodeSystemUnexpected[] = "system_unexpected";

base::DictValue BuildVerificationFailure(std::string error,
                                         std::string error_code) {
  base::DictValue result;
  result.Set("success", false);
  result.Set("error", error);
  result.Set("errorCode", std::move(error_code));
  return result;
}

// Inlined-here copies of the small helper functions originally defined as
// file-locals in `settings/nexus_settings_handler.cc`. They're trivial
// enough that duplicating here (a few lines each) costs less than
// refactoring them out of the legacy handler — which will be deleted
// entirely in P5 along with the duplicates here. When P5 lands, only the
// versions in this file survive.

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

std::string GetConfiguredModelName(PrefService* prefs,
                                   const std::string& provider_id,
                                   nexus::LlmProvider* provider) {
  base::DictValue config = GetStoredProviderConfigCopy(prefs, provider_id);
  const std::string stored_default_model =
      nexus::GetStoredDefaultModelId(config);
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

// Inlined-here copy of `GetConfiguredBaseUrlFromConfig` from
// `settings/nexus_settings_handler.cc`. Used by the verify path.
std::string GetConfiguredBaseUrlFromConfig(const base::DictValue& config,
                                           nexus::LlmProvider* provider) {
  if (const auto* base_url = config.FindString("base_url");
      base_url && !base_url->empty()) {
    return *base_url;
  }
  return provider ? provider->GetBaseUrl() : std::string();
}

// Inlined-here copy of `MergeProviderConfigUpdates` from
// `settings/nexus_settings_handler.cc`. Same scope-discipline note as
// the other helpers above — duplicated rather than moved to a shared
// header so the new adapter can stand alone before P6 retires the
// legacy handler. When that happens, only this copy survives.
base::DictValue MergeProviderConfigUpdates(PrefService* prefs,
                                           const std::string& provider_id,
                                           const base::DictValue& updates) {
  base::DictValue merged = GetStoredProviderConfigCopy(prefs, provider_id);

  if (const auto* api_key = updates.FindString("api_key");
      api_key && !api_key->empty()) {
    merged.Set("api_key",
               nexus::NexusSecretCrypt::GetInstance()->Encrypt(*api_key));
  }

  if (const auto* account_token = updates.FindString("account_token");
      account_token && !account_token->empty()) {
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

  // The draft's preset User-Agent. Custom providers are probed through a
  // shared backend slot whose stored config carries whatever the last request
  // published there, so the probe must take the draft's own value ('' = host
  // default). Absent (an older guest) keeps the stored one.
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

// Build the dict the verify path Configure()s into the TRANSIENT
// provider. Identical to the legacy `BuildVerificationConfig`: starts
// from the merged updates, then strips the fields that aren't relevant
// to the connectivity probe (model selection, enabled_models,
// last_verification). Keeps base_url + api_key/account_token.
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

}  // namespace

NexusProvidersBridgeAdapter::NexusProvidersBridgeAdapter(
    NexusBridgeResponseDelegate* delegate,
    Profile* profile)
    : ::nexus::generated::ProvidersApiBase(delegate), profile_(profile) {}

NexusProvidersBridgeAdapter::~NexusProvidersBridgeAdapter() = default;

void NexusProvidersBridgeAdapter::HandleListOnUI(
    ::nexus::generated::ListInput /*input*/,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  if (!profile_) {
    std::move(done).Error("LLM_ERROR", "Profile unavailable");
    return;
  }
  LlmBroker* broker = LlmBrokerFactory::GetForProfile(profile_);
  if (!broker) {
    std::move(done).Error("LLM_ERROR", "Nexus LLM Broker is not available");
    return;
  }

  PrefService* prefs = profile_->GetPrefs();
  std::vector<ProviderInfo> providers = broker->GetRegisteredProviders();
  const std::string active_id = broker->GetActiveProviderId();

  base::ListValue provider_list;
  for (const auto& info : providers) {
    LlmProvider* provider = broker->GetProvider(info.id);
    base::DictValue stored_config = GetStoredProviderConfigCopy(prefs, info.id);
    base::DictValue provider_dict;
    provider_dict.Set("id", info.id);
    provider_dict.Set("name", info.name);

    std::string type_string;
    switch (info.type) {
      case LlmProviderType::kCloud:
        type_string = "cloud";
        break;
      case LlmProviderType::kLocal:
        type_string = "local";
        break;
      case LlmProviderType::kProxy:
        type_string = "proxy";
        break;
    }
    provider_dict.Set("type", type_string);
    provider_dict.Set("isConfigured", info.is_configured);
    provider_dict.Set("hasStoredApiKey", HasStoredApiKey(stored_config));
    // Verification state from the broker pref — the `last_verification` stamp
    // the settings "Test connection" probe writes. The React Models page uses
    // this to show "Connected" for a provider configured outside settings.json
    // (launch prefs / legacy path), whose config the page's settings.json
    // `providers.configs` view never carries; without it a working,
    // broker-configured provider showed "No provider configured". Display
    // only — it does not gate the catalog (BuildCatalog's gate is
    // `provider_set_up`).
    bool is_verified = false;
    if (const base::DictValue* last_verification =
            stored_config.FindDict("last_verification")) {
      is_verified = last_verification->Find("verifiedAtMs") != nullptr;
    }
    provider_dict.Set("isVerified", is_verified);

    // OAuth-backed providers render a "Sign in" card instead of
    // base-url/api-key fields. Surface auth mode + live connection state
    // so the WebUI can pick the right UI affordance.
    const bool is_oauth = IsOAuthProvider(info.id);
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
        GetStoredDefaultModelId(stored_config);
    if (!stored_default_model.empty()) {
      provider_dict.Set("defaultModel", stored_default_model);
    }

    const std::string configured_base_url =
        GetConfiguredBaseUrl(prefs, info.id, provider);
    if (!configured_base_url.empty()) {
      provider_dict.Set("baseUrl", configured_base_url);
    }

    // Whether the provider ships a default base URL — gates the Base URL field
    // on the React Models page (providers without one hide the field). A null /
    // unconfigured provider reports false.
    provider_dict.Set("hasDefaultBaseUrl",
                      provider && !provider->GetDefaultBaseUrl().empty());

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
  std::move(done).Success(base::Value(std::move(result)));
}

void NexusProvidersBridgeAdapter::HandleUpdateOnUI(
    ::nexus::generated::UpdateInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  if (!profile_) {
    std::move(done).Error("LLM_ERROR", "Profile unavailable");
    return;
  }
  LlmBroker* broker = LlmBrokerFactory::GetForProfile(profile_);
  if (!broker) {
    std::move(done).Error("LLM_ERROR", "Nexus LLM Broker is not available");
    return;
  }

  LlmProvider* provider = broker->GetProvider(input.providerId);
  if (!provider) {
    base::DictValue result;
    result.Set("success", false);
    result.Set("error", "Unknown provider: " + input.providerId);
    std::move(done).Success(base::Value(std::move(result)));
    return;
  }

  PrefService* prefs = profile_->GetPrefs();
  base::DictValue merged_config =
      MergeProviderConfigUpdates(prefs, input.providerId, input.updates);
  const bool success =
      broker->ConfigureProvider(input.providerId, merged_config);

  if (success && provider->IsConfigured()) {
    // Best-effort: kick the broker's per-provider catalog refresh so the
    // hard-coded TS catalog has an in-memory verification echo. Same
    // call site the legacy handler makes.
    broker->RefreshCatalogForProvider(input.providerId);

    // First-time onboarding: auto-promote to active if no provider was
    // active. Matches the legacy handler.
    if (broker->GetActiveProviderId().empty()) {
      broker->SetActiveProvider(input.providerId);
    }
  }

  // NOTE on `BroadcastEnrichedCatalog_`: the legacy
  // `HandleUpdateProviderConfig` fires `nexusCatalogChanged` to refresh
  // the quick-settings popup. That helper lives on
  // `NexusSettingsHandler`, not the broker — so the new adapter can't
  // fire it directly without a refactor. For now both handlers stay
  // registered: the WebUI calls THIS one (panel bridge), but the
  // quick-settings popup keeps receiving `nexusCatalogChanged` through
  // other call sites (verify, refresh, broker pick events). A follow-up
  // moves the broadcast to the broker so this is eventually clean.

  base::DictValue result;
  result.Set("success", success);
  result.Set("isConfigured", provider->IsConfigured());
  result.Set("baseUrl", provider->GetBaseUrl());
  if (!success) {
    result.Set("error", "Failed to configure provider: " + input.providerId);
  }
  std::move(done).Success(base::Value(std::move(result)));
}

void NexusProvidersBridgeAdapter::HandleRefreshOnUI(
    ::nexus::generated::RefreshInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  if (!profile_) {
    std::move(done).Error("LLM_ERROR", "Profile unavailable");
    return;
  }
  LlmBroker* broker = LlmBrokerFactory::GetForProfile(profile_);
  if (!broker) {
    std::move(done).Error("LLM_ERROR", "Nexus LLM Broker is not available");
    return;
  }

  if (!broker->GetProvider(input.providerId)) {
    base::DictValue result;
    result.Set("success", false);
    result.Set("error", "Unknown provider: " + input.providerId);
    std::move(done).Success(base::Value(std::move(result)));
    return;
  }

  // OAuth providers need a backend-ready token applied before discovery
  // (github-copilot exchanges its durable GitHub OAuth token for a
  // short-lived Copilot session token; chatgpt-pro may be near expiry).
  // Mirrors `NexusSettingsHandler::HandleRefreshProviderModels`.
  if (IsOAuthProvider(input.providerId) && broker->oauth_manager()) {
    broker->oauth_manager()->EnsureFreshToken(
        input.providerId,
        base::BindOnce(&NexusProvidersBridgeAdapter::OnOAuthTokenForRefresh,
                       weak_factory_.GetWeakPtr(),
                       std::move(done),
                       input.providerId));
    return;
  }

  broker->RefreshModels(
      input.providerId,
      base::BindOnce(&NexusProvidersBridgeAdapter::OnRefreshModelsFetched,
                     weak_factory_.GetWeakPtr(), std::move(done)));
}

void NexusProvidersBridgeAdapter::OnOAuthTokenForRefresh(
    ::nexus::BridgeResponseCallback done,
    std::string provider_id,
    bool ok,
    std::string access_token,
    std::string account_id) {
  if (!profile_) {
    std::move(done).Error("LLM_ERROR", "Profile unavailable");
    return;
  }
  LlmBroker* broker = LlmBrokerFactory::GetForProfile(profile_);
  if (!broker) {
    std::move(done).Error("LLM_ERROR", "Nexus LLM Broker is not available");
    return;
  }

  if (!ok || access_token.empty()) {
    base::DictValue result;
    result.Set("success", false);
    result.Set("error",
               "Could not obtain a fresh access token. Please sign in again.");
    std::move(done).Success(base::Value(std::move(result)));
    return;
  }

  // Apply the fresh, backend-ready token in-memory. The OAuth token
  // store already persisted the durable credential; a partial Configure
  // here would clobber default_model et al. if written to prefs.
  if (LlmProvider* provider = broker->GetProvider(provider_id)) {
    base::DictValue config;
    config.Set("api_key", access_token);
    config.Set("account_id", account_id);
    provider->Configure(config);
  }

  broker->RefreshModels(
      provider_id,
      base::BindOnce(&NexusProvidersBridgeAdapter::OnRefreshModelsFetched,
                     weak_factory_.GetWeakPtr(), std::move(done)));
}

void NexusProvidersBridgeAdapter::OnRefreshModelsFetched(
    ::nexus::BridgeResponseCallback done,
    std::vector<nexus::ModelInfo> models) {
  base::DictValue result;
  result.Set("success", true);
  result.Set("models", BuildModelList(models));
  std::move(done).Success(base::Value(std::move(result)));
}

void NexusProvidersBridgeAdapter::HandleVerifyOnUI(
    ::nexus::generated::VerifyInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  if (!profile_) {
    std::move(done).Error("LLM_ERROR", "Profile unavailable");
    return;
  }
  LlmBroker* broker = LlmBrokerFactory::GetForProfile(profile_);
  if (!broker) {
    std::move(done).Error("LLM_ERROR", "Nexus LLM Broker is not available");
    return;
  }

  LlmProvider* persisted_provider = broker->GetProvider(input.providerId);
  if (!persisted_provider) {
    std::move(done).Success(base::Value(BuildVerificationFailure(
        "Unknown provider: " + input.providerId, kErrorCodeSystemUnexpected)));
    return;
  }

  // Create a TRANSIENT provider — separate from the persisted one — so
  // an in-flight verify doesn't mutate the live state. Mirrors
  // `NexusSettingsHandler::HandleVerifyProviderConfig`.
  auto transient_provider = LlmProviderFactory::CreateProvider(
      input.providerId,
      profile_->GetDefaultStoragePartition()
          ->GetURLLoaderFactoryForBrowserProcess());
  if (!transient_provider) {
    std::move(done).Success(base::Value(BuildVerificationFailure(
        "Failed to create provider: " + input.providerId,
        kErrorCodeSystemUnexpected)));
    return;
  }

  PrefService* prefs = profile_->GetPrefs();
  // A factory-fresh provider has no default base URL — the guest-owned one
  // lives on the persisted instance, applied from the published catalog.
  transient_provider->SetDefaultBaseUrl(
      persisted_provider->GetDefaultBaseUrl());
  base::DictValue verification_config =
      BuildVerificationConfig(prefs, input.providerId, input.updates);
  if (!transient_provider->Configure(verification_config)) {
    std::move(done).Success(base::Value(BuildVerificationFailure(
        "Provider is not configured. Check the required connection "
        "fields and try again.",
        kErrorCodeAuthUnauthorized)));
    return;
  }

  const std::string base_url = GetConfiguredBaseUrlFromConfig(
      verification_config, transient_provider.get());

  // Use /v1/models (or provider-equivalent) as the connectivity probe.
  // Exercises the same auth surface as a generation call AND returns
  // the data we need next. Ownership of the transient provider rides
  // along in the callback so it self-destructs when the probe lands.
  LlmProvider* transient_raw = transient_provider.get();
  transient_raw->FetchAvailableModels(
      base::BindOnce(&NexusProvidersBridgeAdapter::OnVerifyModelsFetched,
                     weak_factory_.GetWeakPtr(),
                     std::move(done),
                     input.providerId,
                     base_url,
                     std::move(transient_provider)));
}

void NexusProvidersBridgeAdapter::OnVerifyModelsFetched(
    ::nexus::BridgeResponseCallback done,
    std::string provider_id,
    std::string base_url,
    std::unique_ptr<nexus::LlmProvider> /*transient_provider*/,
    std::vector<nexus::ModelInfo> models) {
  // The transient_provider param is held by-value just to keep the
  // provider alive for the duration of the probe — it's dropped on
  // function exit, same lifetime semantics as the legacy
  // PendingVerification erase().
  if (models.empty()) {
    std::move(done).Success(base::Value(BuildVerificationFailure(
        "Connected, but no models were returned.",
        kErrorCodeDiscoveryNoModels)));
    return;
  }
  const int64_t verified_at_ms =
      base::Time::Now().InMillisecondsSinceUnixEpoch();

  base::DictValue result;
  result.Set("success", true);
  result.Set("providerId", provider_id);
  result.Set("baseUrl", base_url);
  result.Set("verifiedAtMs", static_cast<double>(verified_at_ms));
  result.Set("modelCount", static_cast<int>(models.size()));
  result.Set("models", BuildModelList(models));
  std::move(done).Success(base::Value(std::move(result)));
}

void NexusProvidersBridgeAdapter::HandleBeginOAuthOnUI(
    ::nexus::generated::BeginOAuthInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  if (!profile_) {
    std::move(done).Error("LLM_ERROR", "Profile unavailable");
    return;
  }
  LlmBroker* broker = LlmBrokerFactory::GetForProfile(profile_);
  if (!broker || !broker->oauth_manager()) {
    std::move(done).Error("LLM_ERROR", "OAuth is not available");
    return;
  }

  // Device-code providers (GitHub Copilot, Qwen Code) don't use a
  // redirect: request a device code and return userCode +
  // verificationUri for the UI to display. Background poll runs on
  // the OAuth manager.
  const OAuthProviderConfig* oauth_config =
      GetOAuthProviderConfig(input.providerId);
  if (oauth_config && oauth_config->flow == OAuthFlow::kDeviceCode) {
    broker->oauth_manager()->StartDeviceCodeFlow(
        input.providerId,
        base::BindOnce(
            &NexusProvidersBridgeAdapter::OnDeviceCodeStartedForOAuth,
            weak_factory_.GetWeakPtr(), std::move(done)));
    return;
  }

  // PKCE / redirect flow: get the auth URL and open it in a normal
  // browser window so OAuthRedirectThrottle catches the loopback
  // callback. Identical to the legacy handler.
  const std::string auth_url =
      broker->oauth_manager()->BeginLogin(input.providerId);
  if (auth_url.empty()) {
    std::move(done).Error("LLM_ERROR",
                          "Unknown OAuth provider: " + input.providerId);
    return;
  }

  OpenNexusAuthUrl(profile_, GURL(auth_url));

  base::DictValue result;
  // The wire contract (providerAuth.beginOAuth result, protocol/providerAuth.ts)
  // requires `success`. ModelsSection gates on `!res.success`, so omitting it
  // here made a successful start read as a failure — the device code / poll
  // never appeared even though the auth window had already opened.
  result.Set("success", true);
  result.Set("flow", "pkce");
  result.Set("started", true);
  std::move(done).Success(base::Value(std::move(result)));
}

void NexusProvidersBridgeAdapter::OnDeviceCodeStartedForOAuth(
    ::nexus::BridgeResponseCallback done,
    bool ok,
    nexus::NexusOAuthManager::DeviceCodeInfo info,
    std::string error) {
  if (!ok) {
    std::move(done).Error("LLM_ERROR",
                          error.empty() ? "Failed to start sign-in" : error);
    return;
  }

  // Open verification URI in a normal window so the user can authorize
  // without leaving the device code behind.
  if (profile_ && !info.verification_uri.empty()) {
    OpenNexusAuthUrl(profile_, GURL(info.verification_uri));
  }

  base::DictValue result;
  // See the PKCE path: the contract requires `success` (ModelsSection gates on
  // it before showing the device code + starting the connected-poll).
  result.Set("success", true);
  result.Set("flow", "device-code");
  result.Set("userCode", info.user_code);
  result.Set("verificationUri", info.verification_uri);
  result.Set("expiresIn", info.expires_in);
  std::move(done).Success(base::Value(std::move(result)));
}

void NexusProvidersBridgeAdapter::HandleResetOnUI(
    ::nexus::generated::ResetInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  if (!profile_) {
    std::move(done).Error("LLM_ERROR", "Profile unavailable");
    return;
  }
  LlmBroker* broker = LlmBrokerFactory::GetForProfile(profile_);
  if (!broker) {
    std::move(done).Error("LLM_ERROR", "Nexus LLM Broker is not available");
    return;
  }

  if (!broker->GetProvider(input.providerId)) {
    base::DictValue result;
    result.Set("success", false);
    result.Set("error", "Unknown provider: " + input.providerId);
    std::move(done).Success(base::Value(std::move(result)));
    return;
  }

  PrefService* prefs = profile_->GetPrefs();
  base::DictValue all_configs =
      prefs->GetDict(nexus::prefs::kNexusProviderConfigs).Clone();
  all_configs.Remove(input.providerId);
  prefs->SetDict(nexus::prefs::kNexusProviderConfigs, std::move(all_configs));

  // Drop the cached catalogue so a reset clears every trace — otherwise
  // the next browser start would seed the provider's in-memory catalogue
  // from prefs before it refetched. Same semantics as the legacy handler.
  base::DictValue all_catalogs =
      prefs->GetDict(nexus::prefs::kNexusProviderCatalogs).Clone();
  all_catalogs.Remove(input.providerId);
  prefs->SetDict(nexus::prefs::kNexusProviderCatalogs, std::move(all_catalogs));

  if (broker->GetActiveProviderId() == input.providerId) {
    broker->SetActiveProvider(std::string());
  }
  broker->ReloadProvidersFromPrefs();

  // NOTE on `BroadcastEnrichedCatalog_`: same caveat as `HandleUpdateOnUI`.
  // The legacy chrome.send `HandleResetProviderConfig` re-emits
  // `nexusCatalogChanged` from `NexusSettingsHandler`; the new adapter
  // can't reach that helper. The legacy handler stays registered so the
  // popup still gets refreshed via its broadcast path until P6 moves the
  // broadcast source to the broker.

  base::DictValue result;
  result.Set("success", true);
  result.Set("activeProviderId", broker->GetActiveProviderId());
  std::move(done).Success(base::Value(std::move(result)));
}

void NexusProvidersBridgeAdapter::HandleOauthStatusOnUI(
    ::nexus::generated::OauthStatusInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  if (!profile_) {
    std::move(done).Error("LLM_ERROR", "Profile unavailable");
    return;
  }
  LlmBroker* broker = LlmBrokerFactory::GetForProfile(profile_);

  base::DictValue result;
  if (broker && broker->oauth_manager()) {
    NexusOAuthManager* manager = broker->oauth_manager();
    result.Set("connected", manager->IsConnected(input.providerId));
    const std::string email = manager->GetConnectedEmail(input.providerId);
    if (!email.empty()) {
      result.Set("connectedEmail", email);
    }
  } else {
    result.Set("connected", false);
  }
  std::move(done).Success(base::Value(std::move(result)));
}

void NexusProvidersBridgeAdapter::HandleSetActiveOnUI(
    ::nexus::generated::SetActiveInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  if (!profile_) {
    std::move(done).Error("LLM_ERROR", "Profile unavailable");
    return;
  }
  LlmBroker* broker = LlmBrokerFactory::GetForProfile(profile_);
  if (!broker) {
    std::move(done).Error("LLM_ERROR", "Nexus LLM Broker is not available");
    return;
  }

  // Extract + validate the selection dict — mirrors the legacy
  // `HandlePickNexusSelection` checks (the quick-settings popup
  // populates the dict; e2e harnesses may pass through bad shapes).
  const std::string* provider_id = input.selection.FindString("providerId");
  const std::string* model_id = input.selection.FindString("modelId");
  if (!provider_id || provider_id->empty() || !model_id || model_id->empty()) {
    std::move(done).InvalidArgs(
        "selection requires non-empty providerId+modelId");
    return;
  }
  std::string thinking_tier;
  if (const std::string* tier = input.selection.FindString("thinkingTier")) {
    thinking_tier = *tier;
  }

  LlmBroker::DispatchPickResult dispatch =
      broker->DispatchUserPick(*provider_id, *model_id, thinking_tier);

  base::DictValue result;
  result.Set("ok", dispatch.ok);
  if (!dispatch.ok) {
    result.Set("errorCode", dispatch.error_code);
    result.Set("error", dispatch.error_message);
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
  std::move(done).Success(base::Value(std::move(result)));
}

}  // namespace nexus
