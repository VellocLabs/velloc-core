#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_NODE_REGISTRY_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_NODE_REGISTRY_H_

#include <map>
#include <string>
#include <utility>

namespace nexus {

// Per-session map: APC `content_node_id` (the LLM-facing index) →
// `(document_identifier.serialized_token, tab_id)` plus a small amount
// of diagnostic prose, so the LLM can refer to the same DOM element by
// the same index across turns.
//
// Why content_node_id directly? Because (a) it's stable for a
// document's lifetime in the renderer, (b) the actor proto already
// requires it, and (c) the previous indirection (synthetic element_id
// → triple) invalidated every turn when the registry got Clear()'d
// per observation. With the index model, the LLM can carry intent
// across turns ("click the search box at 18372 again") and a stale
// index surfaces as a clean "element not found / stale" error from
// the actor framework rather than a misdirected click.
//
// Populated by observation actions (screenshot / read_page /
// find_text / and the implicit observation that follows every action
// result) which parse the
// `ActionsResult` proto's `tab_observations[].interactable_elements[]`
// and call `Set(...)` for each one.
//
// Looked up by index-based action helpers (click / input / select /
// scroll_to / scroll / read_element). The action's proto target
// is built from the registry entry:
//
//   click->mutable_target()->set_content_node_id(node_id);
//   click->mutable_target()
//        ->mutable_document_identifier()
//        ->set_serialized_token(entry.document_id);
//   click->set_tab_id(entry.tab_id);
//
// Multi-tab disambiguation: NodeRegistry is keyed by `(index,
// tab_id)` because content_node_id is unique per-document, not
// per-session. The lookup convenience `Find(index)` returns the
// most-recently-registered tab's entry — sufficient for
// single-active-tab flows. Actions that take an explicit `tab_id` use
// `FindIn(index, tab_id)` for unambiguous resolution.
//
// Lifecycle: a member of `BrowserTool` (per-session). Cleared by
// `BrowserTool::OnStop` along with the actor task — indexes from
// stopped sessions are never used for new actions. The per-tab map
// is otherwise ACCUMULATIVE across observations of the same tab —
// new entries with the same key overwrite (typical re-observation
// case); old entries stick around until the tab closes or the
// session stops, so cross-turn references work even if the latest
// observation didn't surface the element again.
class NodeRegistry {
 public:
  // One entry's worth of state. The fields beyond `document_id` /
  // `tab_id` are diagnostic only — they help log lines make sense
  // ("clicking <button> 'Submit'") but don't affect dispatch.
  //
  // Chromium-style requires explicit out-of-line ctor / copy / move /
  // dtor on structs with multiple non-trivial members.
  struct Entry {
    Entry();
    Entry(const Entry&);
    Entry(Entry&&) noexcept;
    Entry& operator=(const Entry&);
    Entry& operator=(Entry&&) noexcept;
    ~Entry();

    std::string document_id;
    int tab_id = 0;
    std::string type;  // e.g. "button", "link", "form_control"
    std::string text;  // visible label / inner text, truncated
  };

  NodeRegistry();
  ~NodeRegistry();

  NodeRegistry(const NodeRegistry&) = delete;
  NodeRegistry& operator=(const NodeRegistry&) = delete;

  // Insert or overwrite the entry for `(index, entry.tab_id)`.
  // Called from the observation-parsing path (every action
  // result builds an `ActionsResult` with `tab_observations`; each
  // interactable element gets a registry entry).
  void Set(int node_id, Entry entry);

  // Lookup with explicit tab disambiguation. Use this when the tool
  // method has a tab_id parameter so the resolution is unambiguous.
  // Returns nullptr if the (index, tab_id) pair was never
  // registered (or got cleared).
  const Entry* FindIn(int node_id, int tab_id) const;

  // Lookup convenience for tools that don't pass a tab_id. Returns
  // the most-recently-registered entry for `index` across all
  // tabs. Sufficient for the common single-active-tab case.
  // Returns nullptr if the index isn't registered for any tab.
  const Entry* Find(int node_id) const;

  // Clear every entry. Called from `BrowserTool::OnStop` so a stopped
  // session's stale indexes can't be used by mistake.
  void Clear();

  // Drop every entry for one tab. Useful when a tab closes — the
  // session continues but that tab's nodes are gone. Phase 1: not
  // wired; the registry just accumulates until session OnStop.
  void ClearTab(int tab_id);

  // Diagnostic accessor.
  std::size_t size() const { return entries_.size(); }

 private:
  // Compound key. Pair ordering keeps lookups by (index, tab_id)
  // O(log n) without a separate per-tab structure.
  using Key = std::pair<int /*node_id*/, int /*tab_id*/>;
  std::map<Key, Entry> entries_;

  // Tracks the most-recently-registered tab per index so `Find` (no
  // tab) returns a deterministic answer when the same index exists
  // in multiple tabs.
  std::map<int /*node_id*/, int /*last_tab_id*/> last_tab_for_node_;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_NODE_REGISTRY_H_
