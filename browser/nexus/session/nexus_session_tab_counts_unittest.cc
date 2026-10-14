// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/session/nexus_session_tab_counts.h"

#include <algorithm>
#include <string>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

// The diff's output order is an implementation detail (changed-or-new
// entries first, then removals); key on session for assertions.
const SessionTabCountsChange* FindChange(
    const std::vector<SessionTabCountsChange>& changes,
    const std::string& session_key) {
  auto it = std::find_if(changes.begin(), changes.end(),
                         [&](const SessionTabCountsChange& change) {
                           return change.session_key == session_key;
                         });
  return it == changes.end() ? nullptr : &*it;
}

TEST(NexusSessionTabCountsTest, BuildFromEmptySnapshot) {
  EXPECT_TRUE(BuildCountsFromSnapshot({}).empty());
}

TEST(NexusSessionTabCountsTest, BuildTalliesNexusAndTotalSeparately) {
  SessionTabCountsMap counts = BuildCountsFromSnapshot({
      {"session-a", /*in_nexus_window=*/true},
      {"session-a", /*in_nexus_window=*/true},
      {"session-a", /*in_nexus_window=*/false},
      {"session-b", /*in_nexus_window=*/false},
  });
  ASSERT_EQ(counts.size(), 2u);
  EXPECT_EQ(counts["session-a"].nexus_count, 2);
  EXPECT_EQ(counts["session-a"].total_count, 3);
  // Separate-window mode shape: tabs exist, none in the Nexus window.
  EXPECT_EQ(counts["session-b"].nexus_count, 0);
  EXPECT_EQ(counts["session-b"].total_count, 1);
}

TEST(NexusSessionTabCountsTest, DiffReportsFirstTab) {
  SessionTabCountsMap old_counts;
  SessionTabCountsMap new_counts =
      BuildCountsFromSnapshot({{"session-a", true}});

  std::vector<SessionTabCountsChange> changes =
      DiffSessionTabCounts(old_counts, new_counts);
  ASSERT_EQ(changes.size(), 1u);
  EXPECT_EQ(changes[0].session_key, "session-a");
  EXPECT_EQ(changes[0].counts.nexus_count, 1);
  EXPECT_EQ(changes[0].counts.total_count, 1);
}

TEST(NexusSessionTabCountsTest, DiffReportsLastTabClosedAsZeroed) {
  SessionTabCountsMap old_counts =
      BuildCountsFromSnapshot({{"session-a", true}});
  SessionTabCountsMap new_counts;

  std::vector<SessionTabCountsChange> changes =
      DiffSessionTabCounts(old_counts, new_counts);
  ASSERT_EQ(changes.size(), 1u);
  EXPECT_EQ(changes[0].session_key, "session-a");
  EXPECT_EQ(changes[0].counts, SessionTabCounts{});
}

TEST(NexusSessionTabCountsTest, DiffReportsNexusToNormalWindowMove) {
  // A tab dragged out of the Nexus window: total stays, nexus drops —
  // the transition that auto-hides the webview_area.
  SessionTabCountsMap old_counts =
      BuildCountsFromSnapshot({{"session-a", true}});
  SessionTabCountsMap new_counts =
      BuildCountsFromSnapshot({{"session-a", false}});

  std::vector<SessionTabCountsChange> changes =
      DiffSessionTabCounts(old_counts, new_counts);
  ASSERT_EQ(changes.size(), 1u);
  EXPECT_EQ(changes[0].counts.nexus_count, 0);
  EXPECT_EQ(changes[0].counts.total_count, 1);
}

TEST(NexusSessionTabCountsTest, DiffIsolatesUnchangedSessions) {
  SessionTabCountsMap old_counts = BuildCountsFromSnapshot({
      {"session-a", true},
      {"session-b", true},
  });
  SessionTabCountsMap new_counts = BuildCountsFromSnapshot({
      {"session-a", true},
      {"session-a", true},
      {"session-b", true},
  });

  std::vector<SessionTabCountsChange> changes =
      DiffSessionTabCounts(old_counts, new_counts);
  ASSERT_EQ(changes.size(), 1u);
  EXPECT_EQ(changes[0].session_key, "session-a");
  EXPECT_EQ(changes[0].counts.nexus_count, 2);
  EXPECT_EQ(FindChange(changes, "session-b"), nullptr);
}

TEST(NexusSessionTabCountsTest, DiffIdenticalMapsIsEmpty) {
  SessionTabCountsMap counts = BuildCountsFromSnapshot({
      {"session-a", true},
      {"session-b", false},
  });
  EXPECT_TRUE(DiffSessionTabCounts(counts, counts).empty());
}

}  // namespace
}  // namespace nexus
