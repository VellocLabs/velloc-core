#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_NODE_REGISTRY_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_NODE_REGISTRY_H_

#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

namespace nexus {

// Per-session map: APC `content_node_id` (the model-facing index) →
// `(document token, tab_id)`, so an index stays valid across turns; a stale
// one fails cleanly. Keyed by `(index, tab_id)`: `Find(index)` returns the
// latest tab's entry, `FindIn` is explicit. Filled from observations;
// accumulative per tab and cleared by `BrowserTool::OnStop`.
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
    // The node's APC content_node_id within `document_id` — the actor
    // target's node. Equals the index for main-document nodes.
    int content_node_id = 0;
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

  // First index handed to a node outside its tab's main document.
  static constexpr int kChildDocumentIndexBase = 1 << 30;

  // Model-facing index for `content_node_id` of `document_id` in `tab_id`.
  // A main-document node keeps its content_node_id. A node of any other
  // document (an iframe) gets an index from `kChildDocumentIndexBase`
  // upward, stable for the session: renderer processes number DOM nodes
  // independently, so a cross-process iframe's ids overlap the main
  // frame's (F-21).
  int AssignIndex(int tab_id,
                  const std::string& document_id,
                  int content_node_id,
                  bool is_main_document);

  // The index `AssignIndex` gave that node, without allocating one;
  // nullopt for a child-document node never assigned.
  std::optional<int> IndexOf(int tab_id,
                             const std::string& document_id,
                             int content_node_id,
                             bool is_main_document) const;

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

  // Child-document indexes by (tab_id, document_id, content_node_id).
  using ChildKey = std::tuple<int, std::string, int>;
  std::map<ChildKey, int> child_indexes_;
  int next_child_index_ = kChildDocumentIndexBase;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_NODE_REGISTRY_H_
