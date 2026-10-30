// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/llm/provider_config.h"

#include "base/values.h"
#include "components/os_crypt/async/browser/test_utils.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/testing_pref_service.h"
#include "custom_browser/browser/nexus/api/llm/llm_pref_names.h"
#include "custom_browser/browser/nexus/nexus_secret_crypt.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

class ProviderConfigTest : public ::testing::Test {
 protected:
  void SetUp() override {
    prefs_.registry()->RegisterDictionaryPref(prefs::kNexusProviderConfigs);
  }

  TestingPrefServiceSimple prefs_;
};

TEST_F(ProviderConfigTest, GetStoredProviderConfigReturnsNullForNullPrefs) {
  EXPECT_EQ(GetStoredProviderConfig(nullptr, "openai"), nullptr);
}

TEST_F(ProviderConfigTest, GetStoredProviderConfigReturnsNullForMissingEntry) {
  EXPECT_EQ(GetStoredProviderConfig(&prefs_, "openai"), nullptr);
}

TEST_F(ProviderConfigTest, GetStoredProviderConfigReturnsEntryWhenPresent) {
  base::DictValue all_configs;
  base::DictValue openai_config;
  openai_config.Set("api_key", "sk-live-xxx");
  openai_config.Set("default_model", "gpt-4o");
  all_configs.Set("openai", std::move(openai_config));
  prefs_.SetDict(prefs::kNexusProviderConfigs, std::move(all_configs));

  const base::DictValue* config = GetStoredProviderConfig(&prefs_, "openai");
  ASSERT_NE(config, nullptr);
  const std::string* api_key = config->FindString("api_key");
  ASSERT_NE(api_key, nullptr);
  EXPECT_EQ(*api_key, "sk-live-xxx");
}

TEST_F(ProviderConfigTest, GetStoredDefaultModelIdPrefersDefaultModelField) {
  base::DictValue config;
  config.Set("default_model", "gpt-4o");
  config.Set("model", "gpt-3.5-turbo");
  EXPECT_EQ(GetStoredDefaultModelId(config), "gpt-4o");
}

TEST_F(ProviderConfigTest, GetStoredDefaultModelIdFallsBackToLegacyModelField) {
  base::DictValue config;
  config.Set("model", "gpt-3.5-turbo");
  EXPECT_EQ(GetStoredDefaultModelId(config), "gpt-3.5-turbo");
}

TEST_F(ProviderConfigTest, GetStoredDefaultModelIdIgnoresEmptyValues) {
  base::DictValue config;
  config.Set("default_model", "");
  config.Set("model", "gpt-3.5-turbo");
  EXPECT_EQ(GetStoredDefaultModelId(config), "gpt-3.5-turbo");
}

TEST_F(ProviderConfigTest, GetStoredDefaultModelIdReturnsEmptyWhenNeitherPresent) {
  base::DictValue config;
  config.Set("api_key", "sk-live-xxx");
  EXPECT_TRUE(GetStoredDefaultModelId(config).empty());
}

// ---- api_key sealing for the pref cache -----------------------------------
// The broker copies settings.json `apiKey` into `nexus.provider_configs` on
// every `model.publishProviderConfigs`. These pin that the pref never ends up
// holding a plaintext key, and that "did the key change?" is judged on the
// secret itself rather than on the (randomised) ciphertext.

class ProviderConfigSealTest : public ProviderConfigTest {
 protected:
  void SetUp() override {
    ProviderConfigTest::SetUp();
    NexusSecretCrypt::GetInstance()->SetEncryptorForTesting(
        os_crypt_async::GetTestEncryptorForTesting());
  }
};

TEST_F(ProviderConfigSealTest, SealApiKeyForPrefEncryptsPlaintext) {
  const std::string sealed = SealApiKeyForPref("sk-live-plain");
  EXPECT_TRUE(NexusSecretCrypt::IsEncrypted(sealed));
  EXPECT_EQ(sealed.find("sk-live-plain"), std::string::npos);
  EXPECT_EQ(NexusSecretCrypt::GetInstance()->Decrypt(sealed), "sk-live-plain");
}

// Named secrets for model.rawStreamOpen: sealed on the way into the pref,
// merged (a host-captured secret survives a publish that lacks it), and
// removable one at a time with an empty value.
TEST_F(ProviderConfigSealTest, MergeStoredSecretsSealsMergesAndRemoves) {
  base::DictValue config;
  base::DictValue* stored = config.EnsureDict("secrets");
  const std::string captured = SealApiKeyForPref("refresh-from-host");
  stored->Set("refresh_token", captured);
  stored->Set("old", SealApiKeyForPref("old-value"));

  base::DictValue settings;
  settings.Set("access_token", "at-plain");
  settings.Set("old", "");
  settings.Set("ignored", 42);
  MergeStoredSecrets(&settings, config);

  const base::DictValue* secrets = config.FindDict("secrets");
  ASSERT_TRUE(secrets);
  const std::string* access = secrets->FindString("access_token");
  ASSERT_TRUE(access);
  EXPECT_TRUE(NexusSecretCrypt::IsEncrypted(*access));
  EXPECT_EQ(NexusSecretCrypt::GetInstance()->Decrypt(*access), "at-plain");
  // Not in this publish: kept byte-for-byte.
  EXPECT_EQ(*secrets->FindString("refresh_token"), captured);
  EXPECT_FALSE(secrets->Find("old"));
  EXPECT_FALSE(secrets->Find("ignored"));

  MergeStoredSecrets(nullptr, config);  // no-op
  EXPECT_EQ(config.FindDict("secrets")->size(), 2u);
}

TEST_F(ProviderConfigSealTest, SealApiKeyForPrefIsIdempotentAndKeepsEmpty) {
  // The guest persists ciphertext after `model.encryptSecret` and republishes
  // that exact string at boot — it must ride through untouched, never
  // double-wrapped.
  const std::string sealed = SealApiKeyForPref("sk-once");
  EXPECT_EQ(SealApiKeyForPref(sealed), sealed);
  EXPECT_EQ(SealApiKeyForPref(std::string()), std::string());
}

TEST_F(ProviderConfigSealTest, StoredApiKeysDifferComparesSecretsNotCiphertext) {
  const std::string sealed_a = SealApiKeyForPref("sk-same");
  const std::string sealed_b = SealApiKeyForPref("sk-same");
  // Randomised encryption: two seals of one key are different strings...
  EXPECT_NE(sealed_a, sealed_b);
  // ...but name the same secret, in every stored-form combination.
  EXPECT_FALSE(StoredApiKeysDiffer(sealed_a, sealed_b));
  EXPECT_FALSE(StoredApiKeysDiffer("sk-same", sealed_a));
  EXPECT_FALSE(StoredApiKeysDiffer(sealed_a, "sk-same"));
  EXPECT_FALSE(StoredApiKeysDiffer("sk-same", "sk-same"));
  // A genuinely different key still differs, sealed or not.
  EXPECT_TRUE(StoredApiKeysDiffer(sealed_a, SealApiKeyForPref("sk-other")));
  EXPECT_TRUE(StoredApiKeysDiffer(sealed_a, "sk-other"));
  EXPECT_TRUE(StoredApiKeysDiffer(std::string(), sealed_a));
}

// While the encryptor is not acquired, Decrypt yields "" for every marked
// value; the compare must NOT collapse two different sealed keys into
// "unchanged" (which would keep a stale verification stamp for a new key).
TEST_F(ProviderConfigSealTest, StoredApiKeysDifferFallsBackToBytesWhenNotReady) {
  const std::string sealed_a = SealApiKeyForPref("sk-one");
  const std::string sealed_b = SealApiKeyForPref("sk-two");
  NexusSecretCrypt::GetInstance()->ClearEncryptorForTesting();
  ASSERT_FALSE(NexusSecretCrypt::GetInstance()->ready());
  EXPECT_TRUE(StoredApiKeysDiffer(sealed_a, sealed_b));
  EXPECT_FALSE(StoredApiKeysDiffer(sealed_a, sealed_a));
  // Not ready: sealing is a passthrough (the broker re-applies via WhenReady).
  EXPECT_EQ(SealApiKeyForPref("sk-plain"), "sk-plain");
}

// ---- preset User-Agent ------------------------------------------------------
// The guest publishes `userAgent` on every provider config, '' for "host
// default". The broker merges a publish onto the stored config key by key, so
// an empty or absent value must REMOVE the stored one — otherwise a provider
// that lost its preset (a custom provider moved off the OpenCode Zen URL, a
// slot re-pointed at another endpoint) keeps sending the old User-Agent.

TEST_F(ProviderConfigTest, SetStoredUserAgentSetsNonEmptyValue) {
  base::DictValue config;
  const std::string ua = "opencode/1.18.32";
  SetStoredUserAgent(&ua, config);
  ASSERT_NE(config.FindString("user_agent"), nullptr);
  EXPECT_EQ(*config.FindString("user_agent"), "opencode/1.18.32");
}

TEST_F(ProviderConfigTest, SetStoredUserAgentEmptyOrAbsentClearsStoredValue) {
  base::DictValue config;
  config.Set("user_agent", "opencode/1.18.32");
  config.Set("base_url", "https://opencode.ai/zen/v1");
  const std::string empty;
  SetStoredUserAgent(&empty, config);
  EXPECT_EQ(config.FindString("user_agent"), nullptr);

  config.Set("user_agent", "opencode/1.18.32");
  SetStoredUserAgent(nullptr, config);
  EXPECT_EQ(config.FindString("user_agent"), nullptr);
  // Only the User-Agent is touched.
  ASSERT_NE(config.FindString("base_url"), nullptr);
  EXPECT_EQ(*config.FindString("base_url"), "https://opencode.ai/zen/v1");
}

// The catalog is the guest's `NexusModelConfigWire`; only providers that
// declare a non-empty `defaultBaseUrl` contribute one.
TEST(ProviderConfigCatalogTest, GetCatalogDefaultBaseUrlsReadsEachProvider) {
  base::DictValue kimi;
  kimi.Set("providerId", "kimi");
  kimi.Set("defaultBaseUrl", "https://api.kimi.com/coding/v1");
  kimi.Set("models", base::ListValue());
  base::DictValue chatgpt_pro;  // OAuth backend: no defaultBaseUrl.
  chatgpt_pro.Set("providerId", "chatgpt-pro");
  base::DictValue blank;
  blank.Set("providerId", "glm");
  blank.Set("defaultBaseUrl", "");
  base::ListValue providers;
  providers.Append(std::move(kimi));
  providers.Append(std::move(chatgpt_pro));
  providers.Append(std::move(blank));
  providers.Append("not-a-dict");
  base::DictValue catalog;
  catalog.Set("providers", std::move(providers));

  const auto defaults = GetCatalogDefaultBaseUrls(catalog);
  ASSERT_EQ(defaults.size(), 1u);
  EXPECT_EQ(defaults.at("kimi"), "https://api.kimi.com/coding/v1");

  EXPECT_TRUE(GetCatalogDefaultBaseUrls(base::DictValue()).empty());
}

// The settings save path used to stamp the compiled-in default into
// `base_url` whenever the field was left empty. That stamp pinned Kimi to
// api.moonshot.cn after the default moved, so it is cleared — while a real
// override, a provider without the stamp, and unrelated fields survive.
TEST(ProviderConfigCatalogTest, ClearBakedDefaultBaseUrlsDropsOnlyTheStamp) {
  base::DictValue kimi;
  kimi.Set("api_key", "sk-kimi");
  kimi.Set("base_url", "https://api.moonshot.cn/v1");
  base::DictValue openai;
  openai.Set("base_url", "https://api.openai.com/v1");
  base::DictValue qwen;  // A real override.
  qwen.Set("base_url", "https://dashscope-intl.aliyuncs.com/compatible-mode/v1");
  base::DictValue custom;  // Not a legacy id, even with a matching URL.
  custom.Set("base_url", "https://api.openai.com/v1");
  base::DictValue all;
  all.Set("kimi", std::move(kimi));
  all.Set("openai", std::move(openai));
  all.Set("qwen", std::move(qwen));
  all.Set("custom-abc", std::move(custom));

  EXPECT_TRUE(ClearBakedDefaultBaseUrls(all));

  EXPECT_EQ(*all.FindDict("kimi")->FindString("base_url"), "");
  ASSERT_NE(all.FindDict("kimi")->FindString("api_key"), nullptr);
  EXPECT_EQ(*all.FindDict("openai")->FindString("base_url"), "");
  EXPECT_EQ(*all.FindDict("qwen")->FindString("base_url"),
            "https://dashscope-intl.aliyuncs.com/compatible-mode/v1");
  EXPECT_EQ(*all.FindDict("custom-abc")->FindString("base_url"),
            "https://api.openai.com/v1");

  // Idempotent: a second pass finds nothing left to clear.
  EXPECT_FALSE(ClearBakedDefaultBaseUrls(all));
}

// A keyless provider saved on its default stores only `base_url: ''` — the
// default is guest-owned and never written into the pref. That entry must
// still read as set up, or Ollama would vanish from the picker; while a
// stream-guard-only entry (pushed for every curated provider at boot) must not.
TEST(ProviderConfigSetupTest, EmptyBaseUrlIsSetupEvidenceStreamGuardsAreNot) {
  base::DictValue saved_on_default;
  saved_on_default.Set("base_url", "");
  EXPECT_TRUE(HasUserProviderSetup(saved_on_default));

  base::DictValue stream_guard_only;
  stream_guard_only.Set("stream_idle_timeout_s", 60);
  stream_guard_only.Set("max_concurrent_streams", 1);
  EXPECT_FALSE(HasUserProviderSetup(stream_guard_only));

  base::DictValue empty_key;
  empty_key.Set("api_key", "");
  EXPECT_FALSE(HasUserProviderSetup(empty_key));

  base::DictValue keyed;
  keyed.Set("api_key", "sk-test");
  EXPECT_TRUE(HasUserProviderSetup(keyed));
}

}  // namespace
}  // namespace nexus
