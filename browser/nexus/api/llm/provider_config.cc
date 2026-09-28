// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/llm/provider_config.h"

#include "base/logging.h"
#include "components/prefs/pref_service.h"
#include "custom_browser/browser/nexus/api/llm/llm_pref_names.h"
#include "custom_browser/browser/nexus/nexus_secret_crypt.h"

namespace nexus {

const base::DictValue* GetStoredProviderConfig(
    PrefService* prefs,
    const std::string& provider_id) {
  if (!prefs) {
    return nullptr;
  }
  const base::DictValue& all_configs =
      prefs->GetDict(prefs::kNexusProviderConfigs);
  return all_configs.FindDict(provider_id);
}

std::string GetStoredDefaultModelId(const base::DictValue& config) {
  if (const auto* default_model = config.FindString("default_model");
      default_model && !default_model->empty()) {
    return *default_model;
  }
  if (const auto* model = config.FindString("model");
      model && !model->empty()) {
    return *model;
  }
  return std::string();
}

std::string SealApiKeyForPref(const std::string& api_key) {
  // Encrypt is idempotent on a marked value and a no-op on empty input.
  NexusSecretCrypt* crypt = NexusSecretCrypt::GetInstance();
  std::string sealed = crypt->Encrypt(api_key);
  // Encrypt degrades to plaintext when the encryptor is not ready (the broker
  // re-applies from WhenReady) — but ALSO when the OS encrypt itself fails.
  // That second case has no retry, so the plaintext would land in the pref.
  // Refuse to store it: the guest still holds the key in settings.json and
  // republishes it on the next boot.
  if (crypt->ready() && !api_key.empty() &&
      !NexusSecretCrypt::IsEncrypted(sealed)) {
    LOG(ERROR) << "[nexus] api_key could not be sealed for the pref cache; "
                  "not storing it";
    return std::string();
  }
  return sealed;
}

bool StoredApiKeysDiffer(const std::string& stored_a,
                         const std::string& stored_b) {
  NexusSecretCrypt* crypt = NexusSecretCrypt::GetInstance();
  // Before the encryptor is acquired, Decrypt returns "" for EVERY marked
  // value, which would make two different sealed keys compare equal (and a
  // sealed key differ from its own legacy plaintext). Fall back to a byte
  // compare in that window; the broker re-applies the publish once the
  // encryptor is ready, so the secret-level compare still runs.
  if (!crypt->ready()) {
    return stored_a != stored_b;
  }
  return crypt->Decrypt(stored_a) != crypt->Decrypt(stored_b);
}

void MergeStoredSecrets(const base::DictValue* settings_secrets,
                        base::DictValue& config) {
  if (!settings_secrets) {
    return;
  }
  base::DictValue* stored = config.EnsureDict("secrets");
  for (const auto [name, value] : *settings_secrets) {
    if (!value.is_string()) {
      continue;
    }
    if (value.GetString().empty()) {
      stored->Remove(name);
    } else {
      stored->Set(name, SealApiKeyForPref(value.GetString()));
    }
  }
}

void SetStoredUserAgent(const std::string* user_agent,
                        base::DictValue& config) {
  if (user_agent && !user_agent->empty()) {
    config.Set("user_agent", *user_agent);
  } else {
    config.Remove("user_agent");
  }
}

bool HasUserProviderSetup(const base::DictValue& config) {
  static constexpr const char* kUserSuppliedStringKeys[] = {
      "api_key", "account_token", "default_model", "model",
  };
  for (const char* key : kUserSuppliedStringKeys) {
    if (const std::string* value = config.FindString(key);
        value && !value->empty()) {
      return true;
    }
  }
  if (config.FindString("base_url")) {
    return true;
  }
  if (const base::ListValue* enabled_models = config.FindList("enabled_models");
      enabled_models && !enabled_models->empty()) {
    return true;
  }
  return config.FindBool("is_custom_provider").value_or(false);
}

base::flat_map<std::string, std::string> GetCatalogDefaultBaseUrls(
    const base::DictValue& catalog) {
  base::flat_map<std::string, std::string> defaults;
  const base::ListValue* providers = catalog.FindList("providers");
  if (!providers) {
    return defaults;
  }
  for (const base::Value& entry : *providers) {
    const base::DictValue* provider = entry.GetIfDict();
    if (!provider) {
      continue;
    }
    const std::string* id = provider->FindString("providerId");
    const std::string* url = provider->FindString("defaultBaseUrl");
    if (id && !id->empty() && url && !url->empty()) {
      defaults[*id] = *url;
    }
  }
  return defaults;
}

bool ClearBakedDefaultBaseUrls(base::DictValue& provider_configs) {
  // The defaults this binary compiled in until they moved to the guest. The
  // settings save path used to write the default into `base_url` whenever
  // the field was left empty, so a stored value equal to one of these is
  // almost always that stamp, not a choice — and it would pin the provider
  // to a URL the guest has since changed (Kimi: api.moonshot.cn → the Kimi
  // Code endpoint). Fixed list: new defaults are never stamped, so it never
  // grows.
  static constexpr struct {
    const char* provider_id;
    const char* base_url;
  } kLegacyDefaults[] = {
      {"anthropic", "https://api.anthropic.com"},
      {"deepseek", "https://api.deepseek.com"},
      {"glm", "https://open.bigmodel.cn/api/paas/v4"},
      {"google", "https://generativelanguage.googleapis.com/v1beta"},
      {"kimi", "https://api.moonshot.cn/v1"},
      {"minimax", "https://api.minimax.io/v1"},
      {"ollama", "http://localhost:11434"},
      {"openai", "https://api.openai.com/v1"},
      {"qwen", "https://dashscope.aliyuncs.com/compatible-mode/v1"},
  };
  bool changed = false;
  for (const auto& legacy : kLegacyDefaults) {
    base::DictValue* config = provider_configs.FindDict(legacy.provider_id);
    if (!config) {
      continue;
    }
    const std::string* base_url = config->FindString("base_url");
    if (base_url && *base_url == legacy.base_url) {
      config->Set("base_url", std::string());
      changed = true;
    }
  }
  return changed;
}

}  // namespace nexus
