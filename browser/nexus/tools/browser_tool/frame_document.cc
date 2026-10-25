// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/tools/browser_tool/frame_document.h"

namespace nexus {

namespace apc = optimization_guide::proto;

std::string ChildDocumentId(const apc::ContentNode& node,
                            const std::string& parent_document_id) {
  if (!node.has_content_attributes()) {
    return parent_document_id;
  }
  const apc::ContentAttributes& attrs = node.content_attributes();
  if (attrs.attribute_type() != apc::CONTENT_ATTRIBUTE_IFRAME ||
      !attrs.has_iframe_data() || !attrs.iframe_data().has_frame_data()) {
    return parent_document_id;
  }
  const apc::FrameData& frame = attrs.iframe_data().frame_data();
  if (!frame.has_document_identifier() ||
      frame.document_identifier().serialized_token().empty()) {
    return parent_document_id;
  }
  return frame.document_identifier().serialized_token();
}

}  // namespace nexus
