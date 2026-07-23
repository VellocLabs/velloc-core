// Copyright 2026 The Custom Browser Authors. All rights reserved.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_ERROR_EXTRACT_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_ERROR_EXTRACT_H_

#include <string>

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

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_ERROR_EXTRACT_H_
