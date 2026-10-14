// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/llm/oauth/oauth_providers.h"

#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

TEST(OAuthProvidersTest, ChatGptProConfigPresentAndComplete) {
  const OAuthProviderConfig* config = GetOAuthProviderConfig("chatgpt-pro");
  ASSERT_NE(config, nullptr);
  EXPECT_EQ(config->id, "chatgpt-pro");
  EXPECT_FALSE(config->display_name.empty());
  EXPECT_FALSE(config->client_id.empty());
  EXPECT_FALSE(config->auth_endpoint.empty());
  EXPECT_FALSE(config->token_endpoint.empty());
  EXPECT_FALSE(config->backend_base_url.empty());
  EXPECT_FALSE(config->scopes.empty());
}

TEST(OAuthProvidersTest, NonOAuthProvidersReturnNull) {
  EXPECT_EQ(GetOAuthProviderConfig("openai"), nullptr);
  EXPECT_EQ(GetOAuthProviderConfig("anthropic"), nullptr);
  EXPECT_EQ(GetOAuthProviderConfig(""), nullptr);
}

TEST(OAuthProvidersTest, IsOAuthProviderMatchesOnlyOAuthIds) {
  EXPECT_TRUE(IsOAuthProvider("chatgpt-pro"));
  EXPECT_FALSE(IsOAuthProvider("openai"));
  EXPECT_FALSE(IsOAuthProvider("ollama"));
}

}  // namespace
}  // namespace nexus
