// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_LABEL_TARGETS_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_LABEL_TARGETS_H_

#include <array>
#include <string>
#include <utility>

#include "base/containers/flat_map.h"
#include "base/functional/function_ref.h"
#include "components/optimization_guide/proto/features/common_quality_data.pb.h"

namespace nexus {

// A control's visible `<label for=...>`: the label's DOM node id, its visible
// box [x, y, w, h], and its visible text.
struct LabelTarget {
  int node_id = 0;
  std::array<int, 4> box{};
  std::string text;
};

// The visible label of each labelled control in one observation's APC tree,
// keyed by the control's document token and DOM node id. Node ids are unique
// within one document only: each iframe document numbers its nodes on its
// own, so a bare id would hand one frame's label to another frame's control.
class LabelTargetIndex {
 public:
  // A label node's visible text.
  using TextOf = base::FunctionRef<std::string(
      const optimization_guide::proto::ContentNode& label)>;

  LabelTargetIndex();
  ~LabelTargetIndex();

  LabelTargetIndex(const LabelTargetIndex&) = delete;
  LabelTargetIndex& operator=(const LabelTargetIndex&) = delete;

  // Replaces the index with the labels under `root`, whose document is
  // `root_document_id`. A label counts only with a non-empty visible box;
  // the first such label of a control wins.
  void Build(const optimization_guide::proto::ContentNode& root,
             const std::string& root_document_id,
             TextOf text_of);

  // The label of the control `node_id` in `document_id`, or null.
  const LabelTarget* Find(const std::string& document_id, int node_id) const;

  size_t size() const { return targets_.size(); }

 private:
  void Collect(const optimization_guide::proto::ContentNode& node,
               const std::string& document_id,
               TextOf text_of);

  base::flat_map<std::pair<std::string, int>, LabelTarget> targets_;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_LABEL_TARGETS_H_
