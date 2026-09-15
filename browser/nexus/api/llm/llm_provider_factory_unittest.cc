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
    // Only the OAuth backends compile a base URL in (it is part of their
    // OAuth config). API-key vendors get theirs from the guest's catalog, and
    // custom endpoints from the user — so a fresh instance has none.
    if (id == "chatgpt-pro" || id == "github-copilot" || id == "qwen-code") {
      EXPECT_FALSE(provider->GetDefaultBaseUrl().empty())
          << "OAuth backend lost its base URL: id=" << id;
    } else {
      EXPECT_TRUE(provider->GetDefaultBaseUrl().empty())
          << "Default base URLs are guest-owned; none may be compiled in: id="
          << id;
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

// Every provider reads the preset User-Agent from its Configure() dict, and a
// later Configure() without one restores the host default — even for a
// provider whose Configure() returns early (Google without an api key), since
// the User-Agent is read before any early return.
TEST(LlmProviderFactoryTest, EveryProviderPinsAndClearsTheUserAgent) {
  for (const std::string& id : LlmProviderFactory::GetRegisteredProviderIds()) {
    auto provider = LlmProviderFactory::CreateProvider(id, NullLoaderFactory());
    ASSERT_NE(provider, nullptr) << id;
    EXPECT_TRUE(provider->user_agent().empty())
        << "a fresh provider must use the host default: id=" << id;

    base::DictValue pinned;
    pinned.Set("base_url", "http://127.0.0.1:8765/v1");
    pinned.Set("user_agent", "opencode/1.18.32");
    provider->Configure(pinned);
    EXPECT_EQ(provider->user_agent(), "opencode/1.18.32") << id;

    base::DictValue cleared;
    cleared.Set("base_url", "http://127.0.0.1:8765/v1");
    provider->Configure(cleared);
    EXPECT_TRUE(provider->user_agent().empty())
        << "a config without user_agent must drop the old one: id=" << id;
  }
}

TEST(LlmProviderFactoryTest, OpenAiCompatProvidersHaveTheirOwnIdentity) {
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

  // Each compat provider's display name is its own, not "OpenAI".
  EXPECT_NE(glm->GetDisplayName(), openai->GetDisplayName());
  EXPECT_NE(kimi->GetDisplayName(), openai->GetDisplayName());
  EXPECT_EQ(deepseek->GetDisplayName(), "DeepSeek");
}

// The default base URL is guest-owned: the broker pushes each provider's
// catalog `defaultBaseUrl` through SetDefaultBaseUrl(). The active URL must
// follow it — including a later change, which is how a moved vendor endpoint
// (Kimi: api.moonshot.cn → the Kimi Code endpoint) reaches every profile —
// but never through a user override; and clearing the override ('' in
// Configure) must land back on the CURRENT default, not a stale one. Covers
// every provider class: OpenAI (and its compat ids), Anthropic, Google,
// Ollama.
TEST(LlmProviderFactoryTest, BaseUrlFollowsGuestDefaultUnlessOverridden) {
  for (const char* id : {"openai", "kimi", "anthropic", "google", "ollama"}) {
    SCOPED_TRACE(id);
    auto provider = LlmProviderFactory::CreateProvider(id, NullLoaderFactory());
    ASSERT_TRUE(provider);
    EXPECT_TRUE(provider->GetBaseUrl().empty());

    provider->SetDefaultBaseUrl("https://default-a.example/v1");
    EXPECT_EQ(provider->GetDefaultBaseUrl(), "https://default-a.example/v1");
    EXPECT_EQ(provider->GetBaseUrl(), "https://default-a.example/v1");

    // A config without base_url (e.g. an OAuth token refresh) keeps it.
    base::DictValue key_only;
    key_only.Set("api_key", "test-key");
    provider->Configure(key_only);
    EXPECT_EQ(provider->GetBaseUrl(), "https://default-a.example/v1");

    provider->SetDefaultBaseUrl("https://default-b.example/v1");
    EXPECT_EQ(provider->GetBaseUrl(), "https://default-b.example/v1");

    base::DictValue override_cfg;
    override_cfg.Set("api_key", "test-key");
    override_cfg.Set("base_url", "https://relay.example/v1");
    provider->Configure(override_cfg);
    EXPECT_EQ(provider->GetBaseUrl(), "https://relay.example/v1");

    // A user override wins over a republished default.
    provider->SetDefaultBaseUrl("https://default-c.example/v1");
    EXPECT_EQ(provider->GetBaseUrl(), "https://relay.example/v1");

    base::DictValue cleared;
    cleared.Set("api_key", "test-key");
    cleared.Set("base_url", "");
    provider->Configure(cleared);
    EXPECT_EQ(provider->GetBaseUrl(), "https://default-c.example/v1");
  }
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
