// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

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

TEST(NodeRegistryTest, MainDocumentNodeKeepsItsContentNodeId) {
  NodeRegistry registry;
  EXPECT_EQ(registry.AssignIndex(1, "main", 30, /*is_main_document=*/true),
            30);
  EXPECT_EQ(registry.IndexOf(1, "main", 30, /*is_main_document=*/true), 30);
}

TEST(NodeRegistryTest, ChildDocumentNodeWithSameIdGetsADistinctIndex) {
  NodeRegistry registry;
  const int main_index = registry.AssignIndex(1, "main", 30, true);
  const int child_index = registry.AssignIndex(1, "child", 30, false);
  EXPECT_NE(child_index, main_index);
  EXPECT_GE(child_index, NodeRegistry::kChildDocumentIndexBase);

  NodeRegistry::Entry main_entry = MakeEntry("main", 1, "button", "Search");
  main_entry.content_node_id = 30;
  registry.Set(main_index, main_entry);
  NodeRegistry::Entry child_entry = MakeEntry("child", 1, "button", "Create");
  child_entry.content_node_id = 30;
  registry.Set(child_index, child_entry);

  const NodeRegistry::Entry* main_found = registry.Find(main_index);
  const NodeRegistry::Entry* child_found = registry.Find(child_index);
  ASSERT_NE(main_found, nullptr);
  ASSERT_NE(child_found, nullptr);
  EXPECT_EQ(main_found->document_id, "main");
  EXPECT_EQ(child_found->document_id, "child");
  EXPECT_EQ(child_found->content_node_id, 30);
  EXPECT_EQ(child_found->text, "Create");
}

TEST(NodeRegistryTest, ChildDocumentIndexIsStableAcrossObservations) {
  NodeRegistry registry;
  const int first = registry.AssignIndex(1, "child", 12, false);
  const int other = registry.AssignIndex(1, "child", 13, false);
  EXPECT_NE(first, other);
  EXPECT_EQ(registry.AssignIndex(1, "child", 12, false), first);
  EXPECT_EQ(registry.IndexOf(1, "child", 12, false), first);
}

TEST(NodeRegistryTest, ChildDocumentsAndTabsDoNotShareIndexes) {
  NodeRegistry registry;
  const int a = registry.AssignIndex(1, "child-a", 5, false);
  const int b = registry.AssignIndex(1, "child-b", 5, false);
  const int other_tab = registry.AssignIndex(2, "child-a", 5, false);
  EXPECT_NE(a, b);
  EXPECT_NE(a, other_tab);
  EXPECT_NE(b, other_tab);
}

TEST(NodeRegistryTest, IndexOfDoesNotAllocate) {
  NodeRegistry registry;
  EXPECT_EQ(registry.IndexOf(1, "child", 5, false), std::nullopt);
  EXPECT_EQ(registry.IndexOf(1, "child", 5, false), std::nullopt);
}

TEST(NodeRegistryTest, ClearTabRetiresThatTabsChildIndexes) {
  NodeRegistry registry;
  const int before = registry.AssignIndex(1, "child", 5, false);
  const int kept = registry.AssignIndex(2, "child", 5, false);
  registry.ClearTab(1);
  EXPECT_EQ(registry.IndexOf(1, "child", 5, false), std::nullopt);
  EXPECT_EQ(registry.IndexOf(2, "child", 5, false), kept);
  // A re-walk after the clear never reuses a retired index.
  EXPECT_NE(registry.AssignIndex(1, "child", 5, false), before);
}

TEST(NodeRegistryTest, ClearDropsChildIndexes) {
  NodeRegistry registry;
  registry.AssignIndex(1, "child", 5, false);
  registry.Clear();
  EXPECT_EQ(registry.IndexOf(1, "child", 5, false), std::nullopt);
}

}  // namespace
}  // namespace nexus
