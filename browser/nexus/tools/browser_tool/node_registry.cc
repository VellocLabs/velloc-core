#include "custom_browser/browser/nexus/tools/browser_tool/node_registry.h"

#include <utility>

namespace nexus {

// =============================================================================
// Entry — out-of-line ctor / copy / move / dtor required by
// chromium-style on structs with multiple non-trivial members.
// =============================================================================

NodeRegistry::Entry::Entry() = default;
NodeRegistry::Entry::Entry(const Entry&) = default;
NodeRegistry::Entry::Entry(Entry&&) noexcept = default;
NodeRegistry::Entry& NodeRegistry::Entry::operator=(const Entry&) = default;
NodeRegistry::Entry& NodeRegistry::Entry::operator=(Entry&&) noexcept = default;
NodeRegistry::Entry::~Entry() = default;

// =============================================================================
// NodeRegistry
// =============================================================================

NodeRegistry::NodeRegistry() = default;
NodeRegistry::~NodeRegistry() = default;

void NodeRegistry::Set(int node_id, Entry entry) {
  const int tab_id = entry.tab_id;
  entries_[{node_id, tab_id}] = std::move(entry);
  last_tab_for_node_[node_id] = tab_id;
}

const NodeRegistry::Entry* NodeRegistry::FindIn(int node_id, int tab_id) const {
  auto it = entries_.find({node_id, tab_id});
  return it == entries_.end() ? nullptr : &it->second;
}

const NodeRegistry::Entry* NodeRegistry::Find(int node_id) const {
  auto last_it = last_tab_for_node_.find(node_id);
  if (last_it == last_tab_for_node_.end()) {
    return nullptr;
  }
  return FindIn(node_id, last_it->second);
}

int NodeRegistry::AssignIndex(int tab_id,
                              const std::string& document_id,
                              int content_node_id,
                              bool is_main_document) {
  if (is_main_document) {
    return content_node_id;
  }
  auto [it, inserted] = child_indexes_.try_emplace(
      ChildKey(tab_id, document_id, content_node_id), next_child_index_);
  if (inserted) {
    ++next_child_index_;
  }
  return it->second;
}

std::optional<int> NodeRegistry::IndexOf(int tab_id,
                                         const std::string& document_id,
                                         int content_node_id,
                                         bool is_main_document) const {
  if (is_main_document) {
    return content_node_id;
  }
  auto it =
      child_indexes_.find(ChildKey(tab_id, document_id, content_node_id));
  if (it == child_indexes_.end()) {
    return std::nullopt;
  }
  return it->second;
}

void NodeRegistry::Clear() {
  entries_.clear();
  last_tab_for_node_.clear();
  child_indexes_.clear();
}

void NodeRegistry::ClearTab(int tab_id) {
  // std::map allows mutation during iteration if we advance the
  // iterator before erasing the prior key. Walk all entries and
  // drop those matching tab_id.
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (it->first.second == tab_id) {
      it = entries_.erase(it);
    } else {
      ++it;
    }
  }
  // last_tab_for_node_ may now point at a closed tab. Drop those
  // entries too — a subsequent Find will simply return nullptr until
  // the node reappears in another observation.
  for (auto it = last_tab_for_node_.begin(); it != last_tab_for_node_.end();) {
    if (it->second == tab_id) {
      it = last_tab_for_node_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = child_indexes_.begin(); it != child_indexes_.end();) {
    if (std::get<0>(it->first) == tab_id) {
      it = child_indexes_.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace nexus
