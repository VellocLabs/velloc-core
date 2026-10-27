// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/tools/browser_tool/label_targets.h"

#include "custom_browser/browser/nexus/tools/browser_tool/frame_document.h"

namespace nexus {

namespace apc = optimization_guide::proto;

LabelTargetIndex::LabelTargetIndex() = default;
LabelTargetIndex::~LabelTargetIndex() = default;

void LabelTargetIndex::Build(const apc::ContentNode& root,
                             const std::string& root_document_id,
                             TextOf text_of) {
  targets_.clear();
  Collect(root, root_document_id, text_of);
}

const LabelTarget* LabelTargetIndex::Find(const std::string& document_id,
                                          int node_id) const {
  auto it = targets_.find(std::make_pair(document_id, node_id));
  return it != targets_.end() ? &it->second : nullptr;
}

void LabelTargetIndex::Collect(const apc::ContentNode& node,
                               const std::string& document_id,
                               TextOf text_of) {
  if (node.has_content_attributes()) {
    const apc::ContentAttributes& attrs = node.content_attributes();
    if (attrs.has_label_for_dom_node_id() &&
        attrs.has_common_ancestor_dom_node_id() && attrs.has_geometry() &&
        attrs.geometry().has_visible_bounding_box()) {
      const apc::BoundingRect& r = attrs.geometry().visible_bounding_box();
      auto key = std::make_pair(document_id, attrs.label_for_dom_node_id());
      if ((r.width() > 0 || r.height() > 0) && !targets_.contains(key)) {
        targets_.emplace(
            std::move(key),
            LabelTarget{attrs.common_ancestor_dom_node_id(),
                        {r.x(), r.y(), r.width(), r.height()},
                        text_of(node)});
      }
    }
  }
  const std::string child_document_id = ChildDocumentId(node, document_id);
  for (const apc::ContentNode& child : node.children_nodes()) {
    Collect(child, child_document_id, text_of);
  }
}

}  // namespace nexus
