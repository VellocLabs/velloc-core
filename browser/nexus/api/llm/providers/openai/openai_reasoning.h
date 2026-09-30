// Copyright 2026 The Custom Browser Authors. All rights reserved.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_OPENAI_OPENAI_REASONING_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_OPENAI_OPENAI_REASONING_H_

#include <map>
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

// Rebuilds the complete `reasoning_details` array of one /chat/completions
// response from its streamed fragments, so it can be replayed on the
// assistant message of the next request. OpenRouter and Vercel AI Gateway
// require that replay across tool rounds: it carries the Anthropic thinking
// signatures and the encrypted OpenAI / Gemini reasoning, and without it the
// upstream drops or rejects the continued reasoning.
//
// Fragments are merged by their `index` (arrival order when absent). For an
// index already seen, `text` / `summary` / `data` are appended, and every
// other key (`signature`, `id`, `format`, `type`) takes the latest non-empty
// value — the signature typically arrives on the last fragment.
class ChatCompletionReasoningDetailsAccumulator {
 public:
  ChatCompletionReasoningDetailsAccumulator();
  ChatCompletionReasoningDetailsAccumulator(
      const ChatCompletionReasoningDetailsAccumulator&) = delete;
  ChatCompletionReasoningDetailsAccumulator& operator=(
      const ChatCompletionReasoningDetailsAccumulator&) = delete;
  ~ChatCompletionReasoningDetailsAccumulator();

  // Folds in the `reasoning_details` of one streaming `delta`, or of a whole
  // non-streaming `message`. A chunk without the key is a no-op.
  void Add(const base::DictValue& delta);

  bool empty() const { return details_.empty(); }

  // The merged array, in index order. Leaves the accumulator empty.
  base::ListValue Take();

 private:
  // Keyed by index; std::map keeps them ordered.
  std::map<int, base::DictValue> details_;
  int next_implicit_index_ = 0;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_OPENAI_OPENAI_REASONING_H_
