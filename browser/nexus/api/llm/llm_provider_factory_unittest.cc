// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/llm/llm_provider_factory.h"

#include <algorithm>
#include <memory>
#include <string>

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
    EXPECT_FALSE(provider->GetDefaultBaseUrl().empty())
        << "DefaultBaseUrl was empty for id=" << id;
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

}  // namespace
}  // namespace nexus
