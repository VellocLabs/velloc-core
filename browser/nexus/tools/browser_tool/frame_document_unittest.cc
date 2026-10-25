// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/tools/browser_tool/frame_document.h"

#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

namespace apc = optimization_guide::proto;

apc::ContentNode MakeIframe(const std::string& token) {
  apc::ContentNode node;
  apc::ContentAttributes* attrs = node.mutable_content_attributes();
  attrs->set_attribute_type(apc::CONTENT_ATTRIBUTE_IFRAME);
  attrs->mutable_iframe_data()
      ->mutable_frame_data()
      ->mutable_document_identifier()
      ->set_serialized_token(token);
  return node;
}

TEST(ChildDocumentIdTest, IframeStartsItsOwnDocument) {
  EXPECT_EQ(ChildDocumentId(MakeIframe("child-doc"), "main-doc"), "child-doc");
}

TEST(ChildDocumentIdTest, NestedIframesEachUseTheirOwnFrame) {
  apc::ContentNode outer = MakeIframe("outer-doc");
  const std::string outer_doc = ChildDocumentId(outer, "main-doc");
  EXPECT_EQ(ChildDocumentId(MakeIframe("inner-doc"), outer_doc), "inner-doc");
}

TEST(ChildDocumentIdTest, NonIframeInheritsParentDocument) {
  apc::ContentNode node;
  node.mutable_content_attributes()->set_attribute_type(
      apc::CONTENT_ATTRIBUTE_CONTAINER);
  EXPECT_EQ(ChildDocumentId(node, "main-doc"), "main-doc");
}

TEST(ChildDocumentIdTest, NodeWithoutAttributesInheritsParentDocument) {
  EXPECT_EQ(ChildDocumentId(apc::ContentNode(), "main-doc"), "main-doc");
}

TEST(ChildDocumentIdTest, RedactedIframeInheritsParentDocument) {
  apc::ContentNode node;
  apc::ContentAttributes* attrs = node.mutable_content_attributes();
  attrs->set_attribute_type(apc::CONTENT_ATTRIBUTE_IFRAME);
  attrs->mutable_iframe_data()->mutable_redacted_frame_metadata();
  EXPECT_EQ(ChildDocumentId(node, "main-doc"), "main-doc");
}

TEST(ChildDocumentIdTest, IframeWithEmptyTokenInheritsParentDocument) {
  EXPECT_EQ(ChildDocumentId(MakeIframe(""), "main-doc"), "main-doc");
}

}  // namespace
}  // namespace nexus
