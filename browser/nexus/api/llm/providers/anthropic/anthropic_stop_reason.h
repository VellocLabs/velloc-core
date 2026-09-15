// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_ANTHROPIC_ANTHROPIC_STOP_REASON_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_ANTHROPIC_ANTHROPIC_STOP_REASON_H_

#include <optional>
#include <string_view>

#include "base/values.h"
#include "custom_browser/browser/nexus/api/llm/llm_types.h"

namespace nexus {

// Maps an Anthropic Messages-API `stop_reason` onto the provider-neutral
// FinishReason. Pulled out of anthropic_provider.cc so the mapping is unit
// testable without the network stack.
//
//   end_turn / stop_sequence -> kStop
//   max_tokens               -> kLength
//   tool_use                 -> kToolCalls
//   refusal                  -> kContentFilter  ("content_filter" on the
//                               bridge wire; the guest renders the
//                               categorised refusal card)
//   anything else            -> kOther          ("unknown" on the wire)
//
// `refusal` is the shape Claude Fable 5 / 5.1 / Opus 5 use when a safety
// classifier (or the model itself) declines: HTTP 200, `stop_reason:
// "refusal"`, optional `stop_details`. It is NOT an API error — the request
// was well-formed and billed only for whatever streamed before the stop.
FinishReason TranslateAnthropicStopReason(std::string_view stop_reason);

// Normalizes the `stop_details` object that accompanies `stop_reason:
// "refusal"` into the dict carried on the finish chunk / assistant message:
//
//   {"type": "refusal", "category": "cyber", "explanation": "…",
//    "recommended_model": "…"}
//
// Only string-valued fields are copied (Anthropic documents `category` and
// `explanation` as nullable; the fallback `recommended_model` is a hint that
// rides along untouched). `stop_details` is informational — the guest must
// branch on `stop_reason`, never on this — so a null / absent / non-dict
// value, or a dict with none of the known fields, yields nullopt and the
// finish chunk simply carries no details.
std::optional<base::DictValue> NormalizeAnthropicStopDetails(
    const base::Value* stop_details);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_ANTHROPIC_ANTHROPIC_STOP_REASON_H_
