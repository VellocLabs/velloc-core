// Copyright 2026 The Custom Browser Authors. All rights reserved.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_REASONING_REPLAY_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_REASONING_REPLAY_H_

#include <optional>
#include <string>
#include <string_view>

#include "base/values.h"

namespace nexus {

// Reasoning a provider must receive back on the assistant message of the next
// request — today the `reasoning_details` array OpenRouter / Vercel AI
// Gateway return — carried through the guest inside a thinking block's
// `signature`.
//
// The guest treats `signature` as opaque (it already holds Anthropic's thinking
// signatures there), so the replay payload rides the existing bridge field
// and the transcript with no schema change. The envelope binds the payload to
// the provider and model that produced it:
//
//   velloc-replay-v1:{"provider":"…","model":"…","field":"…","value":…}
//
// and the host hands it back only to that same provider + model. After a
// mid-session model or provider switch the old blocks are silently skipped,
// so no backend is ever sent another backend's signatures (which some
// validate and reject with a 400).
//
// The guest keeps thinking blocks whose signature carries this prefix when
// it builds a request (utils/normalizeMessages.ts); every other thinking
// block is still stripped, as in claude_code.
inline constexpr std::string_view kReasoningReplayPrefix = "velloc-replay-v1:";

// Stream / result part a provider emits to request a replay:
//   {type: "reasoning-replay", field: "<assistant-message key>", value: …}
// and the same shape the host puts back into the prompt for that provider.
inline constexpr char kReasoningReplayPartType[] = "reasoning-replay";

// Wraps a provider's `reasoning-replay` part into a signature string.
// Returns nullopt when the part is malformed (no `field` / no `value`).
std::optional<std::string> EncodeReasoningReplaySignature(
    std::string_view provider_id,
    std::string_view model_id,
    const base::DictValue& replay_part);

// Unwraps a thinking block's signature into a `reasoning-replay` prompt part,
// but only when it was produced by `provider_id` + `model_id`. Returns
// nullopt for any other signature (Anthropic's, another provider's, another
// model's, or garbage).
std::optional<base::DictValue> DecodeReasoningReplaySignatureFor(
    std::string_view signature,
    std::string_view provider_id,
    std::string_view model_id);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_REASONING_REPLAY_H_
