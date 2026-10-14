// Copyright 2026 VellocLabs
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/llm/oauth/oauth_token_store.h"

#include "base/values.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/testing_pref_service.h"
#include "custom_browser/browser/nexus/api/llm/llm_pref_names.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

class OAuthTokenStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    prefs_.registry()->RegisterDictionaryPref(prefs::kNexusProviderConfigs);
  }

  TestingPrefServiceSimple prefs_;
};

TEST_F(OAuthTokenStoreTest, GetReturnsNulloptWhenAbsent) {
  OAuthTokenStore store(&prefs_);
  EXPECT_FALSE(store.GetTokens("chatgpt-pro").has_value());
  EXPECT_FALSE(store.HasTokens("chatgpt-pro"));
}

TEST_F(OAuthTokenStoreTest, SaveAndGetRoundTrip) {
  OAuthTokenStore store(&prefs_);
  OAuthTokens tokens;
  tokens.access_token = "access-123";
  tokens.refresh_token = "refresh-456";
  tokens.expires_at_ms = 1737000000000;
  tokens.account_id = "acct-1";
  tokens.email = "user@example.com";
  store.SaveTokens("chatgpt-pro", tokens);

  std::optional<OAuthTokens> got = store.GetTokens("chatgpt-pro");
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->access_token, "access-123");
  EXPECT_EQ(got->refresh_token, "refresh-456");
  EXPECT_EQ(got->expires_at_ms, 1737000000000);
  EXPECT_EQ(got->account_id, "acct-1");
  EXPECT_EQ(got->email, "user@example.com");
  EXPECT_TRUE(store.HasTokens("chatgpt-pro"));
}

TEST_F(OAuthTokenStoreTest, SaveMirrorsApiKeyAndStampsVerification) {
  OAuthTokenStore store(&prefs_);
  OAuthTokens tokens;
  tokens.access_token = "secret-access";
  tokens.account_id = "acct-9";
  store.SaveTokens("chatgpt-pro", tokens);

  const base::DictValue& all = prefs_.GetDict(prefs::kNexusProviderConfigs);
  const base::DictValue* config = all.FindDict("chatgpt-pro");
  ASSERT_NE(config, nullptr);

  // Access token mirrored into api_key so the standard provider path works.
  const std::string* api_key = config->FindString("api_key");
  ASSERT_NE(api_key, nullptr);
  EXPECT_EQ(*api_key, "secret-access");

  // Account id mirrored into account_id so Configure() sends the
  // ChatGPT-Account-Id header after a prefs reconfigure.
  const std::string* account_id = config->FindString("account_id");
  ASSERT_NE(account_id, nullptr);
  EXPECT_EQ(*account_id, "acct-9");

  // last_verification stamped so BuildCatalog marks the provider enabled.
  const base::DictValue* verification = config->FindDict("last_verification");
  ASSERT_NE(verification, nullptr);
  EXPECT_NE(verification->Find("verifiedAtMs"), nullptr);
  const std::string* provider_id = verification->FindString("providerId");
  ASSERT_NE(provider_id, nullptr);
  EXPECT_EQ(*provider_id, "chatgpt-pro");
}

TEST_F(OAuthTokenStoreTest, ResourceUrlRoundTripAndNormalizedBaseUrlMirror) {
  OAuthTokenStore store(&prefs_);
  OAuthTokens tokens;
  tokens.access_token = "qwen-access";
  tokens.resource_url = "portal.qwen.ai";  // bare host
  store.SaveTokens("qwen-code", tokens);

  std::optional<OAuthTokens> got = store.GetTokens("qwen-code");
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->resource_url, "portal.qwen.ai");

  // A bare host is normalized to https://<host>/v1 and mirrored into base_url
  // so the provider talks to the account's endpoint.
  const base::DictValue* config =
      prefs_.GetDict(prefs::kNexusProviderConfigs).FindDict("qwen-code");
  ASSERT_NE(config, nullptr);
  const std::string* base_url = config->FindString("base_url");
  ASSERT_NE(base_url, nullptr);
  EXPECT_EQ(*base_url, "https://portal.qwen.ai/v1");
}

TEST_F(OAuthTokenStoreTest, ResourceUrlAlreadyNormalizedIsIdempotent) {
  OAuthTokenStore store(&prefs_);
  OAuthTokens tokens;
  tokens.access_token = "qwen-access";
  tokens.resource_url = "https://example.com/v1/";  // scheme + /v1 + slash
  store.SaveTokens("qwen-code", tokens);

  const base::DictValue* config =
      prefs_.GetDict(prefs::kNexusProviderConfigs).FindDict("qwen-code");
  ASSERT_NE(config, nullptr);
  const std::string* base_url = config->FindString("base_url");
  ASSERT_NE(base_url, nullptr);
  EXPECT_EQ(*base_url, "https://example.com/v1");
}

TEST_F(OAuthTokenStoreTest, AbsentResourceUrlLeavesBaseUrlUnset) {
  OAuthTokenStore store(&prefs_);
  OAuthTokens tokens;
  tokens.access_token = "secret-access";
  store.SaveTokens("chatgpt-pro", tokens);

  const base::DictValue* config =
      prefs_.GetDict(prefs::kNexusProviderConfigs).FindDict("chatgpt-pro");
  ASSERT_NE(config, nullptr);
  EXPECT_EQ(config->FindString("base_url"), nullptr);
}

TEST_F(OAuthTokenStoreTest, DeleteRemovesEntry) {
  OAuthTokenStore store(&prefs_);
  OAuthTokens tokens;
  tokens.access_token = "access-123";
  store.SaveTokens("chatgpt-pro", tokens);
  ASSERT_TRUE(store.HasTokens("chatgpt-pro"));

  store.DeleteTokens("chatgpt-pro");
  EXPECT_FALSE(store.HasTokens("chatgpt-pro"));
}

}  // namespace
}  // namespace nexus
