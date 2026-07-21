// Copyright 2026 The Custom Browser Authors. All rights reserved.

#include "custom_browser/browser/nexus/api/llm/providers/openai/openai_usage.h"

#include "base/values.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

// Regression: OpenAI-compatible `prompt_tokens` is INCLUSIVE of cached tokens.
// The bridge expects cache-EXCLUSIVE input, so the cached subset must be
// subtracted — otherwise the composer cache ratio double-counts cache and pins
// at ~50% even when ~all input is served from cache.
TEST(OpenAiUsageTest, SubtractsCachedSubsetFromPromptTokens) {
  base::DictValue usage;
  usage.Set("prompt_tokens", 1000);
  usage.Set("completion_tokens", 250);
  base::DictValue details;
  details.Set("cached_tokens", 990);
  usage.Set("prompt_tokens_details", std::move(details));

  base::DictValue out = NormalizeChatCompletionUsage(usage);

  EXPECT_EQ(out.FindInt("promptTokens"), 10);
  EXPECT_EQ(out.FindInt("cacheReadInputTokens"), 990);
  EXPECT_EQ(out.FindInt("completionTokens"), 250);
}

// With no cache the prompt count passes through unchanged and no cache field is
// emitted.
TEST(OpenAiUsageTest, NoCachePassesThrough) {
  base::DictValue usage;
  usage.Set("prompt_tokens", 500);
  usage.Set("completion_tokens", 40);

  base::DictValue out = NormalizeChatCompletionUsage(usage);

  EXPECT_EQ(out.FindInt("promptTokens"), 500);
  EXPECT_EQ(out.FindInt("completionTokens"), 40);
  EXPECT_FALSE(out.contains("cacheReadInputTokens"));
}

// A malformed server that reports more cached than prompt tokens must not emit
// a negative prompt count.
TEST(OpenAiUsageTest, ClampsNegativePromptToZero) {
  base::DictValue usage;
  usage.Set("prompt_tokens", 100);
  base::DictValue details;
  details.Set("cached_tokens", 150);
  usage.Set("prompt_tokens_details", std::move(details));

  base::DictValue out = NormalizeChatCompletionUsage(usage);

  EXPECT_EQ(out.FindInt("promptTokens"), 0);
  EXPECT_EQ(out.FindInt("cacheReadInputTokens"), 150);
}

}  // namespace
}  // namespace nexus
