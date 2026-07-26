// Copyright 2026 The Custom Browser Authors. All rights reserved.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_ERROR_EXTRACT_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_ERROR_EXTRACT_H_

#include <optional>
#include <string>
#include <string_view>

namespace nexus {

// Extracts a human-readable error message from an LLM backend error body.
// Shared by every provider's HTTP-error path (OpenAI-compatible,
// Anthropic, Google, Ollama). Recognizes the common envelope shapes —
// `{"error":{"message":...}}` (OpenAI / Anthropic / Gemini),
// `{"error":"..."}`, `{"detail":"..."}` (the ChatGPT/Codex backend),
// `{"message":"..."}` — delivered either as a raw JSON body OR wrapped
// in an SSE `data:` frame. The frame matters: some gateways (the
// DashScope / token-plan Coding endpoint, live-verified 2026-07-23;
// Anthropic-compatible proxies) return HTTP 4xx errors as a single
// `data: {"error":{...}}` event, and without unwrapping it the provider
// can only surface a bare "HTTP <status>" with no detail. Returns empty
// when the body isn't a recognizable error object (a normal SSE stream,
// a successful response, non-JSON), so callers fall back to their own
// status-only message.
std::string ExtractApiErrorMessage(const std::string& body);

// Builds the canonical provider-error string every provider hands to its
// done/generate callback, and the ONLY channel the guest has for classifying
// a failure: the host↔guest LLM contract carries errors as a plain string, so
// anything not in the text (status code, Retry-After) is lost.
//
// Before this helper each provider dropped the status whenever the body had an
// extractable message, so `classifyProviderConnectivityIssue`
// (nexus_web/src/utils/providerConnectivity.ts) had to guess from wording, and
// `parseRetryAfterMs` — which already looks for "retry-after <n>" — could never
// match because nothing ever emitted it.
//
// Format (each part omitted when unavailable):
//   "HTTP <status>: <message> (retry-after: <n>s)"
// `retry_after_header` is the raw `Retry-After` value; only the delta-seconds
// form is forwarded (the HTTP-date form is rare from LLM APIs and the guest
// parses seconds).
std::string BuildProviderErrorMessage(int http_status,
                                      std::string_view retry_after_header,
                                      std::string_view message);

// Structured fields recovered from a BuildProviderErrorMessage string.
struct ParsedProviderError {
  // 100-599 when the message starts with the canonical "HTTP <status>"
  // prefix; nullopt otherwise.
  std::optional<int> http_status;
  // Non-negative seconds when the message ends with the canonical
  // "(retry-after: <n>s)" suffix; nullopt otherwise.
  std::optional<int> retry_after_seconds;
};

// The tested inverse of BuildProviderErrorMessage. The provider→host error
// hop is a plain string (LlmProvider callback signatures), so this is where
// the host recovers the typed fields before putting them on the wire
// (NexusLlmStreamEvent.error_http_status / error_retry_after_seconds) —
// letting the GUEST classify retries off typed fields instead of regexing
// display text. Only the canonical anchored shapes are recognized; a bare
// 3-digit number elsewhere in the message is never treated as a status.
// Round-trip pinned by llm_error_extract_unittest.cc: Parse(Build(s, r, m))
// always recovers exactly {s, r}.
ParsedProviderError ParseProviderErrorMessage(std::string_view message);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_ERROR_EXTRACT_H_
