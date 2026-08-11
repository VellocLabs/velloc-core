// Copyright 2026 The Custom Browser Authors. All rights reserved.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_OPENAI_OPENAI_RESPONSES_ERROR_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_OPENAI_OPENAI_RESPONSES_ERROR_H_

#include <string>

#include "base/values.h"

namespace nexus {

// ---------------------------------------------------------------------------
// Responses-API terminal error events
// ---------------------------------------------------------------------------
//
// The public /v1/responses API and the ChatGPT subscription (Codex) backend
// disagree about where a terminal error's message lives:
//
//   - `response.failed` nests it at `response.error.message` (documented);
//   - a bare `error` event carries it flat at `message` (documented);
//   - the Codex backend instead reuses OpenAI's NON-streaming envelope on
//     that same `error` event — `{"error":{"code","message","param","type"},
//     "sequence_number":N,"type":"error"}` (observed live 2026-08-11 on
//     chatgpt-pro / gpt-5.6-luna).
//
// The third shape used to fall through to a bare "Responses API stream error"
// placeholder, throwing the provider's own explanation away. That is not a
// cosmetic loss: the guest classifies retryability off this very text
// (`classifyProviderConnectivityIssue`), the placeholder matches none of its
// patterns, so it returned null → NOT retryable → a transient capacity
// outage surfaced as an un-actionable "Issue: unknown" card that was never
// retried even once.
//
// Worse, the backend emits BOTH events for one failure — `error` first, then
// `response.failed` ~15ms later — and the host queues only the FIRST terminal
// event. So the uninformative one won the race and the good one was dropped.
// Extracting correctly from either shape removes the race entirely.

// Longest serialized event inlined into an error message. Long enough for any
// real error envelope, short enough that a pathological payload cannot flood
// the transcript it ends up in.
inline constexpr size_t kMaxSerializedErrorEventChars = 1024;

// Serialize an event for diagnosis, bounded. Never returns empty.
std::string SerializeEventForDiagnosis(const base::DictValue& data);

// The human-readable message from a Responses-API terminal error event, or
// empty when no known shape matched.
std::string ExtractResponsesApiErrorMessage(const base::DictValue& data);

// The machine-readable code beside the message when the backend supplies one
// ("server_is_overloaded", "rate_limit_exceeded", …), or empty. Prefixed onto
// the message by the caller so the guest's classifier has a stable token to
// match instead of free-form prose.
std::string ExtractResponsesApiErrorCode(const base::DictValue& data);

// Compose the message the provider hands to the bridge for a terminal error
// event: the extracted message, prefixed with its code when one is present
// and not already spelled in the message; or, when NO known shape matched,
// the serialized event itself rather than a placeholder. Never empty.
// `out_recognized` (optional) receives whether a known shape matched.
std::string BuildResponsesApiErrorMessage(const base::DictValue& data,
                                          bool* out_recognized);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_OPENAI_OPENAI_RESPONSES_ERROR_H_
