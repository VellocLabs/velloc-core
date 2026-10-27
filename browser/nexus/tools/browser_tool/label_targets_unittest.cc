// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/tools/browser_tool/label_targets.h"

#include <string>

#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

namespace apc = optimization_guide::proto;

// A `<label for=control_id>` node with a visible box of `width` x 10.
void AddLabel(apc::ContentNode* parent,
              int label_id,
              int control_id,
              int width = 40) {
  apc::ContentAttributes* attrs =
      parent->add_children_nodes()->mutable_content_attributes();
  attrs->set_common_ancestor_dom_node_id(label_id);
  attrs->set_label_for_dom_node_id(control_id);
  apc::BoundingRect* box =
      attrs->mutable_geometry()->mutable_visible_bounding_box();
  box->set_x(1);
  box->set_y(2);
  box->set_width(width);
  box->set_height(width ? 10 : 0);
}

apc::ContentNode* AddIframe(apc::ContentNode* parent,
                            const std::string& token) {
  apc::ContentNode* node = parent->add_children_nodes();
  apc::ContentAttributes* attrs = node->mutable_content_attributes();
  attrs->set_attribute_type(apc::CONTENT_ATTRIBUTE_IFRAME);
  attrs->mutable_iframe_data()
      ->mutable_frame_data()
      ->mutable_document_identifier()
      ->set_serialized_token(token);
  return node;
}

const auto LabelText = [](const apc::ContentNode& label) {
  return "label-" +
         std::to_string(label.content_attributes().common_ancestor_dom_node_id());
};

TEST(LabelTargetIndexTest, TheSameNodeIdInTwoDocumentsKeepsEachLabel) {
  apc::ContentNode root;
  AddLabel(&root, /*label_id=*/20, /*control_id=*/7);
  AddLabel(AddIframe(&root, "child-doc"), /*label_id=*/30, /*control_id=*/7);

  LabelTargetIndex index;
  index.Build(root, "main-doc", LabelText);
  ASSERT_EQ(2u, index.size());
  const LabelTarget* main = index.Find("main-doc", 7);
  ASSERT_TRUE(main);
  EXPECT_EQ(20, main->node_id);
  EXPECT_EQ("label-20", main->text);
  const LabelTarget* child = index.Find("child-doc", 7);
  ASSERT_TRUE(child);
  EXPECT_EQ(30, child->node_id);
  EXPECT_EQ(nullptr, index.Find("other-doc", 7));
}

TEST(LabelTargetIndexTest, AFrameLabelDoesNotLabelAMainFrameControl) {
  apc::ContentNode root;
  AddLabel(AddIframe(&root, "child-doc"), /*label_id=*/30, /*control_id=*/7);

  LabelTargetIndex index;
  index.Build(root, "main-doc", LabelText);
  EXPECT_EQ(nullptr, index.Find("main-doc", 7));
  EXPECT_TRUE(index.Find("child-doc", 7));
}

TEST(LabelTargetIndexTest, FirstVisibleLabelWins) {
  apc::ContentNode root;
  AddLabel(&root, /*label_id=*/19, /*control_id=*/7, /*width=*/0);
  AddLabel(&root, /*label_id=*/20, /*control_id=*/7);
  AddLabel(&root, /*label_id=*/21, /*control_id=*/7);

  LabelTargetIndex index;
  index.Build(root, "main-doc", LabelText);
  const LabelTarget* target = index.Find("main-doc", 7);
  ASSERT_TRUE(target);
  EXPECT_EQ(20, target->node_id);
  EXPECT_EQ(1, target->box[0]);
  EXPECT_EQ(40, target->box[2]);
}

TEST(LabelTargetIndexTest, BuildReplacesTheEarlierIndex) {
  apc::ContentNode first;
  AddLabel(&first, /*label_id=*/20, /*control_id=*/7);
  LabelTargetIndex index;
  index.Build(first, "main-doc", LabelText);
  ASSERT_TRUE(index.Find("main-doc", 7));

  index.Build(apc::ContentNode(), "main-doc", LabelText);
  EXPECT_EQ(0u, index.size());
}

}  // namespace
}  // namespace nexus
