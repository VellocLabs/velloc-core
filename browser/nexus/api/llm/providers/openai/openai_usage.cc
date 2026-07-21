// Copyright 2026 The Custom Browser Authors. All rights reserved.

#include "custom_browser/browser/nexus/api/llm/providers/openai/openai_usage.h"

namespace nexus {

base::DictValue NormalizeChatCompletionUsage(const base::DictValue& usage) {
  base::DictValue u;
  const int prompt = usage.FindInt("prompt_tokens").value_or(0);
  u.Set("completionTokens", usage.FindInt("completion_tokens").value_or(0));
  int cached = 0;
  if (const base::DictValue* details =
          usage.FindDict("prompt_tokens_details")) {
    cached = details->FindInt("cached_tokens").value_or(0);
  }
  if (cached > 0) {
    u.Set("cacheReadInputTokens", cached);
  }
  u.Set("promptTokens", prompt > cached ? prompt - cached : 0);
  return u;
}

}  // namespace nexus
