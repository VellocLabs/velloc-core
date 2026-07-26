// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/llm/llm_provider_factory.h"

#include <algorithm>
#include <memory>
#include <string>

#include "base/values.h"
#include "custom_browser/browser/nexus/api/llm/llm_provider.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

// The factory creates providers without touching the network during
// construction, so nullptr for the URLLoaderFactory is sufficient here.
scoped_refptr<network::SharedURLLoaderFactory> NullLoaderFactory() {
  return nullptr;
}

TEST(LlmProviderFactoryTest, CreateKnownProvidersReturnsInstanceWithMatchingId) {
  for (const std::string& id : LlmProviderFactory::GetRegisteredProviderIds()) {
    auto provider = LlmProviderFactory::CreateProvider(id, NullLoaderFactory());
    ASSERT_NE(provider, nullptr) << "CreateProvider returned null for id=" << id;
    EXPECT_EQ(provider->GetId(), id);
    EXPECT_FALSE(provider->GetDisplayName().empty())
        << "DisplayName was empty for id=" << id;
    // Custom endpoints intentionally ship no default base URL — the user
    // supplies it in Settings → Models. Every built-in provider has a default.
    if (id == "custom-openai" || id == "custom-anthropic") {
      EXPECT_TRUE(provider->GetDefaultBaseUrl().empty())
          << "Custom endpoint should have no default base URL: id=" << id;
    } else {
      EXPECT_FALSE(provider->GetDefaultBaseUrl().empty())
          << "DefaultBaseUrl was empty for id=" << id;
    }
  }
}

TEST(LlmProviderFactoryTest, CreateUnknownProviderReturnsNull) {
  EXPECT_EQ(LlmProviderFactory::CreateProvider("", NullLoaderFactory()),
            nullptr);
  EXPECT_EQ(LlmProviderFactory::CreateProvider("not-a-provider",
                                               NullLoaderFactory()),
            nullptr);
  EXPECT_EQ(
      LlmProviderFactory::CreateProvider("OpenAI", NullLoaderFactory()),
      nullptr)
      << "Provider IDs are case-sensitive";
}

TEST(LlmProviderFactoryTest, RegisteredIdsIncludeAllExpectedProviders) {
  auto ids = LlmProviderFactory::GetRegisteredProviderIds();
  auto contains = [&ids](const std::string& id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
  };

  EXPECT_TRUE(contains("openai"));
  EXPECT_TRUE(contains("anthropic"));
  EXPECT_TRUE(contains("google"));
  EXPECT_TRUE(contains("ollama"));
  // OpenAI-compatible providers — these were previously separate classes
  // but are now created directly from OpenAiProvider's compat constructor.
  EXPECT_TRUE(contains("glm"));
  EXPECT_TRUE(contains("kimi"));
  EXPECT_TRUE(contains("minimax"));
  EXPECT_TRUE(contains("qwen"));
  EXPECT_TRUE(contains("deepseek"));
  // User-configurable custom endpoints (one per wire dialect).
  EXPECT_TRUE(contains("custom-openai"));
  EXPECT_TRUE(contains("custom-anthropic"));
}

TEST(LlmProviderFactoryTest, CustomProvidersHaveExpectedIdentity) {
  auto openai =
      LlmProviderFactory::CreateProvider("custom-openai", NullLoaderFactory());
  auto anthropic = LlmProviderFactory::CreateProvider("custom-anthropic",
                                                       NullLoaderFactory());
  ASSERT_TRUE(openai && anthropic);

  EXPECT_EQ(openai->GetId(), "custom-openai");
  EXPECT_EQ(openai->GetDisplayName(), "Custom (OpenAI-compatible)");
  EXPECT_TRUE(openai->GetDefaultBaseUrl().empty());

  EXPECT_EQ(anthropic->GetId(), "custom-anthropic");
  EXPECT_EQ(anthropic->GetDisplayName(), "Custom (Anthropic-compatible)");
  EXPECT_TRUE(anthropic->GetDefaultBaseUrl().empty());
}

// Custom endpoints run keyless: unconfigured until a base URL is set, then
// configured without any API key (many compatible proxies need none).
TEST(LlmProviderFactoryTest, CustomProvidersAreKeylessConfigurable) {
  auto openai =
      LlmProviderFactory::CreateProvider("custom-openai", NullLoaderFactory());
  ASSERT_TRUE(openai);
  EXPECT_FALSE(openai->IsConfigured());
  base::DictValue openai_cfg;
  openai_cfg.Set("base_url", "http://127.0.0.1:8765/v1");
  EXPECT_TRUE(openai->Configure(openai_cfg));
  EXPECT_TRUE(openai->IsConfigured());
  EXPECT_EQ(openai->GetBaseUrl(), "http://127.0.0.1:8765/v1");

  auto anthropic = LlmProviderFactory::CreateProvider("custom-anthropic",
                                                      NullLoaderFactory());
  ASSERT_TRUE(anthropic);
  EXPECT_FALSE(anthropic->IsConfigured());
  base::DictValue anthropic_cfg;
  anthropic_cfg.Set("base_url", "http://127.0.0.1:8765");
  EXPECT_TRUE(anthropic->Configure(anthropic_cfg));
  EXPECT_TRUE(anthropic->IsConfigured());
  EXPECT_EQ(anthropic->GetBaseUrl(), "http://127.0.0.1:8765");
}

TEST(LlmProviderFactoryTest, OpenAiCompatProvidersHaveDistinctBaseUrls) {
  auto openai =
      LlmProviderFactory::CreateProvider("openai", NullLoaderFactory());
  auto glm = LlmProviderFactory::CreateProvider("glm", NullLoaderFactory());
  auto kimi = LlmProviderFactory::CreateProvider("kimi", NullLoaderFactory());
  auto minimax =
      LlmProviderFactory::CreateProvider("minimax", NullLoaderFactory());
  auto qwen = LlmProviderFactory::CreateProvider("qwen", NullLoaderFactory());
  auto deepseek =
      LlmProviderFactory::CreateProvider("deepseek", NullLoaderFactory());

  ASSERT_TRUE(openai && glm && kimi && minimax && qwen && deepseek);

  EXPECT_NE(glm->GetDefaultBaseUrl(), openai->GetDefaultBaseUrl());
  EXPECT_NE(kimi->GetDefaultBaseUrl(), openai->GetDefaultBaseUrl());
  EXPECT_NE(minimax->GetDefaultBaseUrl(), openai->GetDefaultBaseUrl());
  EXPECT_NE(qwen->GetDefaultBaseUrl(), openai->GetDefaultBaseUrl());
  EXPECT_NE(deepseek->GetDefaultBaseUrl(), openai->GetDefaultBaseUrl());

  // And each compat provider's display name is its own, not "OpenAI".
  EXPECT_NE(glm->GetDisplayName(), openai->GetDisplayName());
  EXPECT_NE(kimi->GetDisplayName(), openai->GetDisplayName());
  EXPECT_EQ(deepseek->GetDisplayName(), "DeepSeek");
}

// --- User-created custom providers -----------------------------------------
// A provider the user adds in Settings has no compiled-in id, so the factory
// must be able to build one from (id, name, dialect) alone. Without this the
// broker cannot materialise it, `GetProvider()` returns null, and every
// request addressed to it dies.

TEST(LlmProviderFactoryTest, CreateCustomProviderBuildsPerDialectInstances) {
  auto openai = LlmProviderFactory::CreateCustomProvider(
      "custom-abc-provider", "ABC Provider", "openai", NullLoaderFactory());
  ASSERT_TRUE(openai);
  EXPECT_EQ(openai->GetId(), "custom-abc-provider");
  EXPECT_EQ(openai->GetDisplayName(), "ABC Provider");
  // Same shape as the compiled-in custom endpoints: no default URL, no key.
  EXPECT_TRUE(openai->GetDefaultBaseUrl().empty());
  EXPECT_FALSE(openai->IsConfigured());

  auto anthropic = LlmProviderFactory::CreateCustomProvider(
      "custom-gateway", "Internal Gateway", "anthropic", NullLoaderFactory());
  ASSERT_TRUE(anthropic);
  EXPECT_EQ(anthropic->GetId(), "custom-gateway");
  EXPECT_EQ(anthropic->GetDisplayName(), "Internal Gateway");
  EXPECT_TRUE(anthropic->GetDefaultBaseUrl().empty());
}

// Two custom providers on the SAME dialect must be independent instances —
// that independence is the whole point of building them per-provider instead
// of re-pointing one shared endpoint slot at each in turn.
TEST(LlmProviderFactoryTest, CustomProvidersOnOneDialectDoNotShareConfig) {
  auto first = LlmProviderFactory::CreateCustomProvider(
      "custom-abc-provider", "ABC Provider", "openai", NullLoaderFactory());
  auto second = LlmProviderFactory::CreateCustomProvider(
      "custom-gateway", "Internal Gateway", "openai", NullLoaderFactory());
  ASSERT_TRUE(first && second);

  base::DictValue first_cfg;
  first_cfg.Set("base_url", "https://api.abc.example/v1");
  first_cfg.Set("api_key", "sk-abc");
  ASSERT_TRUE(first->Configure(first_cfg));

  base::DictValue second_cfg;
  second_cfg.Set("base_url", "http://gateway.internal/v1");
  ASSERT_TRUE(second->Configure(second_cfg));

  EXPECT_EQ(first->GetBaseUrl(), "https://api.abc.example/v1");
  EXPECT_EQ(second->GetBaseUrl(), "http://gateway.internal/v1");
}

TEST(LlmProviderFactoryTest, CreateCustomProviderRejectsBadInput) {
  // Unknown dialect: refuse rather than guess a wire format.
  EXPECT_EQ(LlmProviderFactory::CreateCustomProvider(
                "custom-x", "X", "grpc", NullLoaderFactory()),
            nullptr);
  EXPECT_EQ(LlmProviderFactory::CreateCustomProvider(
                "custom-x", "X", "", NullLoaderFactory()),
            nullptr);
  EXPECT_EQ(LlmProviderFactory::CreateCustomProvider(
                "", "X", "openai", NullLoaderFactory()),
            nullptr);
}

// A provider whose name the user left blank still needs SOME display name;
// falling back to the id keeps the settings list and error messages readable.
TEST(LlmProviderFactoryTest, CustomProviderFallsBackToIdForDisplayName) {
  auto provider = LlmProviderFactory::CreateCustomProvider(
      "custom-unnamed", "", "openai", NullLoaderFactory());
  ASSERT_TRUE(provider);
  EXPECT_EQ(provider->GetDisplayName(), "custom-unnamed");
}

}  // namespace
}  // namespace nexus
