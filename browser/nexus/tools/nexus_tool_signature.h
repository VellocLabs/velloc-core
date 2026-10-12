#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_NEXUS_TOOL_SIGNATURE_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_NEXUS_TOOL_SIGNATURE_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/values.h"

namespace nexus {

// Agent-loop metadata per tool invocation — the `agent_meta` envelope the
// guest reads for loop detection (`action_hashes[]`, `page_fingerprint`) and
// page-change guards. No legacy single `action_hash`.
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

// Canonical action signatures for `action_hashes[]`, one per action item:
//   search → "search|<engine>|<sorted-unique-tokens>"; click/select/hover →
//   "click|<index>"; input → "input|<index>|<trimmed-lowercase-text>";
//   navigate/create_tab → "navigate|<url-without-fragment>"; scroll →
//   "scroll|<direction>|<index>"; drag → "drag|<from_index>|<to_index>";
//   wait/done/back exempt; else "<type>|<stable-json-without-nulls>".
// Returns 12-char SHA-256 hex prefixes; exempt items are omitted.
std::vector<std::string> ComputeActionHashes(std::string_view tool_name,
                                             const base::DictValue& args);

// Whether this tool ends a multi-action batch. Source-of-truth for
// `terminates_sequence` used by the multi-act guard.
bool IsSequenceTerminator(std::string_view tool_name);

// The page fingerprint dom_hash: a 16-char SHA-256 hex prefix of the
// normalized interactive-surface projection, without tab/document ids.
std::string ComputeDomHash(std::string_view normalized_projection);

// Wraps an observation as `{data, agent_meta: {action_hashes,
// page_fingerprint, post_url, post_focus_target_id, terminates_sequence}}`;
// unset fields are omitted.
base::Value WrapResultWithAgentMeta(base::Value observation,
                                    const AgentToolMeta& meta);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_NEXUS_TOOL_SIGNATURE_H_
