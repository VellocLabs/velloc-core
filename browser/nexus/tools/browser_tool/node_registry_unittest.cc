// Copyright 2026 VellocLabs
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/tools/browser_tool/node_registry.h"

#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

NodeRegistry::Entry MakeEntry(const std::string& doc,
                              int tab,
                              const std::string& type,
                              const std::string& text) {
  NodeRegistry::Entry entry;
  entry.document_id = doc;
  entry.tab_id = tab;
  entry.type = type;
  entry.text = text;
  return entry;
}

TEST(NodeRegistryTest, EmptyRegistryReturnsNullOnFind) {
  NodeRegistry registry;
  EXPECT_EQ(registry.size(), 0u);
  EXPECT_EQ(registry.Find(1), nullptr);
  EXPECT_EQ(registry.FindIn(1, 1), nullptr);
  EXPECT_EQ(registry.Find(0), nullptr);
}

TEST(NodeRegistryTest, SetThenFindByNodeReturnsEntry) {
  NodeRegistry registry;
  registry.Set(/*node_id=*/100,
               MakeEntry("doc-token-A", /*tab=*/7, "button", "Submit"));
  EXPECT_EQ(registry.size(), 1u);

  const NodeRegistry::Entry* entry = registry.Find(100);
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->document_id, "doc-token-A");
  EXPECT_EQ(entry->tab_id, 7);
  EXPECT_EQ(entry->type, "button");
  EXPECT_EQ(entry->text, "Submit");
}

TEST(NodeRegistryTest, FindInResolvesByCompoundKey) {
  NodeRegistry registry;
  registry.Set(100, MakeEntry("doc-A", /*tab=*/1, "button", "A"));
  registry.Set(100, MakeEntry("doc-B", /*tab=*/2, "link", "B"));
  EXPECT_EQ(registry.size(), 2u);

  const NodeRegistry::Entry* a = registry.FindIn(100, 1);
  const NodeRegistry::Entry* b = registry.FindIn(100, 2);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(a->document_id, "doc-A");
  EXPECT_EQ(a->type, "button");
  EXPECT_EQ(b->document_id, "doc-B");
  EXPECT_EQ(b->type, "link");

  // FindIn with no matching tab returns nullptr.
  EXPECT_EQ(registry.FindIn(100, 99), nullptr);
}

TEST(NodeRegistryTest, FindWithoutTabReturnsMostRecent) {
  NodeRegistry registry;
  registry.Set(100, MakeEntry("doc-A", /*tab=*/1, "button", "A"));
  registry.Set(100, MakeEntry("doc-B", /*tab=*/2, "link", "B"));

  // Most recent insertion wins for the convenience Find().
  const NodeRegistry::Entry* found = registry.Find(100);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->tab_id, 2);
  EXPECT_EQ(found->document_id, "doc-B");
}

TEST(NodeRegistryTest, SetOverwritesSameKey) {
  NodeRegistry registry;
  registry.Set(42, MakeEntry("doc-A", 7, "button", "Submit"));
  registry.Set(42, MakeEntry("doc-B", 7, "link", "Cancel"));

  // Same (node_id, tab_id) → overwrite, not duplicate.
  EXPECT_EQ(registry.size(), 1u);

  const NodeRegistry::Entry* entry = registry.FindIn(42, 7);
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->document_id, "doc-B");
  EXPECT_EQ(entry->type, "link");
  EXPECT_EQ(entry->text, "Cancel");
}

TEST(NodeRegistryTest, MultipleDistinctNodesCoexist) {
  NodeRegistry registry;
  registry.Set(10, MakeEntry("doc-1", 1, "button", "A"));
  registry.Set(20, MakeEntry("doc-2", 1, "link", "B"));
  registry.Set(30, MakeEntry("doc-3", 2, "form_control", "C"));
  EXPECT_EQ(registry.size(), 3u);

  EXPECT_EQ(registry.Find(10)->document_id, "doc-1");
  EXPECT_EQ(registry.Find(20)->type, "link");
  EXPECT_EQ(registry.Find(30)->tab_id, 2);
  EXPECT_EQ(registry.Find(40), nullptr);
}

TEST(NodeRegistryTest, ClearRemovesAllEntries) {
  NodeRegistry registry;
  registry.Set(10, MakeEntry("doc-1", 1, "button", "A"));
  registry.Set(20, MakeEntry("doc-2", 1, "link", "B"));
  ASSERT_EQ(registry.size(), 2u);

  registry.Clear();
  EXPECT_EQ(registry.size(), 0u);
  EXPECT_EQ(registry.Find(10), nullptr);
  EXPECT_EQ(registry.Find(20), nullptr);
}

TEST(NodeRegistryTest, ClearTabDropsOnlyMatchingTab) {
  NodeRegistry registry;
  registry.Set(10, MakeEntry("doc-1", 1, "button", "A"));
  registry.Set(20, MakeEntry("doc-2", 2, "link", "B"));
  registry.Set(30, MakeEntry("doc-3", 1, "form_control", "C"));
  ASSERT_EQ(registry.size(), 3u);

  registry.ClearTab(1);
  EXPECT_EQ(registry.size(), 1u);
  EXPECT_EQ(registry.Find(10), nullptr);
  EXPECT_EQ(registry.Find(30), nullptr);

  const NodeRegistry::Entry* survivor = registry.Find(20);
  ASSERT_NE(survivor, nullptr);
  EXPECT_EQ(survivor->tab_id, 2);
}

TEST(NodeRegistryTest, EmptyDiagnosticFieldsAreAllowed) {
  NodeRegistry registry;
  // The framework's observation parser may populate type/text only
  // when the actor surfaces them; the registry must still hold the
  // entry usefully when those fields are blank.
  NodeRegistry::Entry entry;
  entry.document_id = "doc";
  entry.tab_id = 1;
  // type and text left default-empty.
  registry.Set(7, std::move(entry));

  const NodeRegistry::Entry* found = registry.Find(7);
  ASSERT_NE(found, nullptr);
  EXPECT_TRUE(found->type.empty());
  EXPECT_TRUE(found->text.empty());
}

}  // namespace
}  // namespace nexus
