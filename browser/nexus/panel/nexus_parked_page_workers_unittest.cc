// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/panel/nexus_parked_page_workers.h"

#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/tokens/tokens.h"

namespace nexus {
namespace {

using Token = blink::DedicatedWorkerToken;

// The set reports only its empty / not-empty edges.
TEST(ParkedWorkerSetTest, ReportsOnlyItsEdges) {
  ParkedWorkerSet set;
  const Token a, b;
  EXPECT_TRUE(set.empty());
  EXPECT_TRUE(set.AddFromPage(a));
  EXPECT_FALSE(set.AddFromPage(b));
  EXPECT_FALSE(set.AddFromPage(a));
  EXPECT_FALSE(set.Remove(a));
  EXPECT_FALSE(set.empty());
  EXPECT_TRUE(set.Remove(b));
  EXPECT_TRUE(set.empty());
  EXPECT_FALSE(set.Remove(b));
}

// A worker its parent created counts once the parent is in, whichever is
// reported first, and outlives the parent.
TEST(ParkedWorkerSetTest, NestedWorkerCountsInEitherOrder) {
  const Token parent, child;

  ParkedWorkerSet parent_first;
  EXPECT_TRUE(parent_first.AddFromPage(parent));
  EXPECT_FALSE(parent_first.AddNested(child, parent));
  EXPECT_FALSE(parent_first.Remove(parent));
  EXPECT_FALSE(parent_first.empty()) << "the child still runs";
  EXPECT_TRUE(parent_first.Remove(child));

  ParkedWorkerSet child_first;
  EXPECT_FALSE(child_first.AddNested(child, parent));
  EXPECT_TRUE(child_first.empty()) << "its parent is not known yet";
  EXPECT_TRUE(child_first.AddFromPage(parent));
  EXPECT_FALSE(child_first.Remove(parent));
  EXPECT_FALSE(child_first.empty()) << "the child was admitted with its parent";
  EXPECT_TRUE(child_first.Remove(child));
}

// A worker whose parent never joins the set is not counted, and leaving drops
// it from the waiting list.
TEST(ParkedWorkerSetTest, WorkerOfAnotherPageIsNotCounted) {
  ParkedWorkerSet set;
  const Token stranger_parent, stranger, parent;
  EXPECT_FALSE(set.AddNested(stranger, stranger_parent));
  EXPECT_FALSE(set.Remove(stranger));
  EXPECT_TRUE(set.AddFromPage(parent));
  EXPECT_TRUE(set.Remove(parent));
  EXPECT_TRUE(set.empty());
}

}  // namespace
}  // namespace nexus
