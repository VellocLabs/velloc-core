// Copyright 2026 The Custom Browser Authors. All rights reserved.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_ANTHROPIC_ANTHROPIC_SCHEMA_SANITIZER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_ANTHROPIC_ANTHROPIC_SCHEMA_SANITIZER_H_

#include "base/values.h"

namespace nexus {

// Rewrites an OpenAI-strict JSON schema into the dialect the Anthropic
// Messages API accepts.
//
// The tool declarations author ONE strict schema per tool, in OpenAI's
// structured-outputs dialect: every property listed in `required`, with
// model-optional fields expressed as nullable-required — `"type": ["T",
// "null"]` and enums carrying a literal `null` member. Anthropic's strict
// tool use rejects that form (no `null` in enums / nullable-required
// unions); it expects plain JSON Schema where optional fields are simply
// absent from `required` — the same shape claude_code emits from zod
// (optionals out of `required`, no null members).
//
// This transform derives that Anthropic form mechanically:
//   - `"type": ["T", "null"]`  →  `"type": "T"`  (drops the null member;
//     a multi-type remainder stays an array),
//   - `"enum": [null, a, b]`   →  `"enum": [a, b]`,
//   - every property that was nullable is removed from the enclosing
//     object's `required` array,
//   - recurses through `properties`, `items`, `$defs`/`definitions`, and
//     the combinators (`anyOf` / `oneOf` / `allOf`).
// Everything else (descriptions, `additionalProperties: false`, property
// set) is preserved, so the model keeps the full parameter surface.
//
// Called on the Anthropic request path only (TranslateTools), and only for
// tools that opted into strict — the permissive schemas already use plain
// optionals and pass through unchanged.
base::DictValue SanitizeStrictSchemaForAnthropic(const base::DictValue& schema);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_ANTHROPIC_ANTHROPIC_SCHEMA_SANITIZER_H_
