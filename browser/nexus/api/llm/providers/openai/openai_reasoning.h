// Copyright 2026 The Custom Browser Authors. All rights reserved.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_OPENAI_OPENAI_REASONING_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_OPENAI_OPENAI_REASONING_H_

#include <string>
#include <string_view>

#include "base/values.h"

namespace nexus {

// Readable reasoning text carried by one OpenAI-compatible /chat/completions
// streaming `delta`, or by a non-streaming `message`. The OpenAI spec has no
// reasoning field, so every backend picked its own; sources in order, and the
// FIRST non-empty one wins:
//
//   1. `reasoning_content` - DeepSeek, Kimi, Qwen (DashScope), GLM, vLLM.
//   2. `reasoning`         - OpenRouter, Vercel AI Gateway, Ollama.
//   3. `reasoning_details` - the gateways' structured form: the `text` of
//      `reasoning.text` entries and the `summary` of `reasoning.summary`
//      entries, concatenated. `reasoning.encrypted` carries nothing readable.
//
// First-wins, not concatenation: the gateways send `reasoning` AND
// `reasoning_details` holding the SAME text on one chunk, and newer vLLM
// sends `reasoning` alongside the deprecated `reasoning_content`. Joining
// them would render every thought twice.
//
// Returns "" when the chunk carries no readable reasoning.
std::string ExtractChatCompletionReasoningText(const base::DictValue& delta);

// Readable text of one Responses API `reasoning` output item (non-streaming
// `output[]`): the `summary[].text` parts, else the raw `content[].text`
// parts, joined by a blank line. "" when the item is encrypted-only.
std::string ExtractResponsesReasoningItemText(const base::DictValue& item);

// True for the Responses API streaming events that carry reasoning text in
// their `delta` field (`response.reasoning_summary_text.delta` and
// `response.reasoning_text.delta`).
bool IsResponsesReasoningTextDeltaEvent(std::string_view event_type);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_OPENAI_OPENAI_REASONING_H_
