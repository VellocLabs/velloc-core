#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_NEXUS_TOOL_SIGNATURE_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_NEXUS_TOOL_SIGNATURE_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/values.h"

namespace nexus {

// Agent-loop metadata produced per tool invocation. Mirrors the
// `agent_meta` envelope consumed by the JS side at
// `nexus_web/src/tools/glic/hostedTools.ts:extractAgentMeta`.
//
// Two consumers on the JS side:
//   - `services/agent/actionLoopDetector` records `action_hashes[]` into
//     a rolling window for soft loop detection and compares
//     `page_fingerprint` against the previous turn's fingerprint to
//     drive stagnation tracking.
//   - `services/tools/toolOrchestration` reads `post_url`,
//     `post_focus_target_id`, and `terminates_sequence` for multi-act
//     page-change guards.
//
// The browser loop consumes `action_hashes[]` as the only loop-history
// hash field. Do not emit a legacy single `action_hash`.
//
// Chromium-style requires explicit out-of-line ctor / copy / move /
// dtor on structs with multiple non-trivial members (the std::string
// fields below trip the rule).
struct AgentToolMeta {
  AgentToolMeta();
  AgentToolMeta(const AgentToolMeta&);
  AgentToolMeta(AgentToolMeta&&) noexcept;
  AgentToolMeta& operator=(const AgentToolMeta&);
  AgentToolMeta& operator=(AgentToolMeta&&) noexcept;
  ~AgentToolMeta();

  // Browser Use records loop history per action item, not per model step.
  // browser_actions therefore exposes one hash per non-exempt action item.
  std::vector<std::string> action_hashes;

  // Page-state hash used for stagnation detection. Computed over a
  // NORMALIZED projection of the interactive surface — type, text,
  // role, form-state, anchor href, walk order — with non-identity
  // bookkeeping (tab_id, document_id) stripped. index is the
  // LLM-facing stable identifier, so its presence in the projection
  // is fine: same DOM = same indexes = same hash.
  std::string page_url;
  int page_interactive_count = 0;
  std::string page_dom_hash;  // 16-char SHA-256 hex prefix.

  // Post-action page state. Used by the JS-side multi-act guard to
  // abort queued tool calls when the page changed mid-batch.
  std::string post_url;
  // Empty string ↔ no focus target; serialized as JS `null`.
  std::string post_focus_target_id;
  bool post_focus_target_present = false;

  // True for actions that must end the queued multi-action batch
  // (navigate / search / back / switch_tab / evaluate / close_tab /
  // done). Consulted by the JS-side multi-act guard.
  bool terminates_sequence = false;
};

// Canonical action signatures for `action_hashes[]`. Mirrors the rules at
// D:/project/broswer_use/browser-use-agent-implementation-report.md
// "Action Normalization":
//   browser_actions       → one hash per action item in actions[]
//   search                → "search|<engine>|<sorted-unique-tokens>"
//   click/select/hover    → "click|<index>"
//   input                 → "input|<index>|<trimmed-lowercase-text>"
//   navigate/create_tab   → "navigate|<url-without-fragment>"
//   scroll/scroll_to      → "scroll|<direction>|<index>"
//   drag_and_drop         → "drag|<from_index>|<to_index>"
//   wait/done/back        → empty (exempt — JS skips recording)
//   default action        → "<type>|<stable-json-without-nulls>"
//
// Returns SHA-256 hex prefixes (12 chars) of the canonical signatures.
// Exempt action items are omitted. Lower-case tokens, trimmed, sorted
// for search queries to keep "openai pricing" and "PRICING openai"
// hash-identical.
std::vector<std::string> ComputeActionHashes(std::string_view tool_name,
                                             const base::DictValue& args);

// Whether this tool ends a multi-action batch. Source-of-truth for
// `terminates_sequence` used by the multi-act guard.
bool IsSequenceTerminator(std::string_view tool_name);

// Build the page fingerprint dom_hash from a normalized projection
// of the interactive surface. Caller assembles type / text / role /
// form-state / href in walk order and passes the joined string here.
// Non-identity fields (tab_id, document_id) are not included so
// re-observations of the same page hash equal even when the bookkeeping
// triples differ. index is stable and may be included or omitted —
// the current caller (BrowserTool::WalkContentNode) omits it because
// it's redundant with the projection's deterministic walk order, but
// including it would also be correct. Returns 16-char SHA-256 hex
// prefix.
std::string ComputeDomHash(std::string_view normalized_projection);

// Wrap a tool's observation result with agent_meta. Output shape
// matches the JS-side `extractAgentMeta` parser:
//   {
//     "data": <observation>,
//     "agent_meta": {
//       "action_hashes": [<12-char hex>, ...],
//       "page_fingerprint": { url, interactive_count, dom_hash },
//       "post_url": <string>,
//       "post_focus_target_id": <string|null>,
//       "terminates_sequence": <bool>
//     }
//   }
//
// Empty / unset fields on `meta` are omitted from the output so older
// JS builds that ignore agent_meta still see the regular observation.
base::Value WrapResultWithAgentMeta(base::Value observation,
                                    const AgentToolMeta& meta);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_NEXUS_TOOL_SIGNATURE_H_
