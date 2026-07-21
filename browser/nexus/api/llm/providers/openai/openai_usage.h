// Copyright 2026 The Custom Browser Authors. All rights reserved.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_OPENAI_OPENAI_USAGE_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_OPENAI_OPENAI_USAGE_H_

#include "base/values.h"

namespace nexus {

// Normalizes an OpenAI-compatible /chat/completions `usage` block into the
// bridge's camelCase stream-usage vocabulary (`promptTokens` /
// `completionTokens` / `cacheReadInputTokens`).
//
// OpenAI-compatible servers report `prompt_tokens` INCLUSIVE of cached tokens,
// with `prompt_tokens_details.cached_tokens` a subset. The bridge follows the
// Anthropic convention where `input_tokens` EXCLUDES cache (the JS token
// counter sums input + cache_read + cache_creation + output), so this emits the
// EXCLUSIVE prompt count. Leaving cache in `promptTokens` double-counts it and
// pins the composer cache ratio at ~50% even when ~all input is cached.
base::DictValue NormalizeChatCompletionUsage(const base::DictValue& usage);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_OPENAI_OPENAI_USAGE_H_
