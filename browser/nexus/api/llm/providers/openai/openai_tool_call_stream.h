// Copyright 2026 The Custom Browser Authors. All rights reserved.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_OPENAI_OPENAI_TOOL_CALL_STREAM_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_OPENAI_OPENAI_TOOL_CALL_STREAM_H_

#include <map>
#include <string>

#include "base/functional/callback.h"
#include "base/values.h"

namespace nexus {

// Translates ONE `delta.tool_calls[i]` entry from an OpenAI-compatible
// /chat/completions SSE chunk into the bridge's stream-chunk vocabulary
// (`tool-call-streaming-start` / `tool-call-delta`), correlating argument
// deltas to their tool call by the SSE `index`.
//
// `tool_call_ids` is the per-stream map of index -> tool-call id; the caller
// owns it and passes the SAME map for every chunk of one response so argument
// deltas can be joined to the id announced by the opening chunk.
//
// The opening `tool-call-streaming-start` is emitted EXACTLY once per index,
// and only for a NON-EMPTY id. This matters for DashScope (Qwen): unlike
// OpenAI — which omits `id` / `function.name` on follow-up deltas — DashScope
// repeats the whole tool-call object on every argument chunk, but with an
// EMPTY `id` ("") and a null `function.name`. `DictValue::FindString` returns
// non-null for an empty string, so a naive "id present?" check re-emits a
// start for every chunk (shattering one call into many nameless "tool" calls
// downstream) and overwrites the correlated id with "" (so the argument deltas
// lose their toolCallId -> "Bridge tool-call parts must include toolCallId").
// Guarding on a non-empty id + opening once per index keeps a single
// well-formed tool call whose argument deltas all carry the real id.
void TranslateChatCompletionToolCallDelta(
    const base::DictValue& tool_call,
    std::map<int, std::string>& tool_call_ids,
    const base::RepeatingCallback<void(base::DictValue)>& emit);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_OPENAI_OPENAI_TOOL_CALL_STREAM_H_
