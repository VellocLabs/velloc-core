// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_FRAME_DOCUMENT_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_FRAME_DOCUMENT_H_

#include <string>

#include "components/optimization_guide/proto/features/common_quality_data.pb.h"

namespace nexus {

// Returns the document token that `node`'s children belong to. An iframe
// node whose APC carries its frame's document identifier starts a new
// document (same-origin or cross-origin); any other node, including a
// redacted iframe, inherits `parent_document_id`. Actor targets pair a
// content_node_id with this token, so a child-frame element dispatched with
// the main frame's token resolves against the wrong document.
std::string ChildDocumentId(
    const optimization_guide::proto::ContentNode& node,
    const std::string& parent_document_id);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_FRAME_DOCUMENT_H_
