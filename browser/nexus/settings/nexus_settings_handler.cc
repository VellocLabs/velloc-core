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
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "custom_browser/browser/nexus/llm/llm_broker.h"
#include "custom_browser/browser/nexus/llm/llm_broker_factory.h"
#include "custom_browser/browser/nexus/llm/llm_pref_names.h"
#include "custom_browser/browser/nexus/llm/llm_provider.h"
#include "custom_browser/browser/nexus/llm/llm_provider_factory.h"
#include "custom_browser/browser/nexus/llm/llm_types.h"
#include "custom_browser/browser/nexus/llm/provider_config.h"

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
}

void NexusSettingsHandler::OnJavascriptAllowed() {}

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
}

}  // namespace settings
