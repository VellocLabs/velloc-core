// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_SESSION_NEXUS_SESSION_TAB_COUNTS_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_SESSION_NEXUS_SESSION_TAB_COUNTS_H_

#include <string>
#include <vector>

#include "base/containers/flat_map.h"

namespace nexus {

// Per-session tab tallies derived from walking every tab that carries a
// NexusSessionTabTag. `nexus_count` is the slice hosted in the profile's
// kNexus window — the number that gates the guest's per-session
// webview_area visibility (the native main-contents-view can only show a
// tab living in the Nexus window's own tab strip). `total_count`
// additionally covers tabs the user dragged into another window and tabs
// created in the separate agent-tabs window
// (`nexus.browser_tools_separate_window`).
struct SessionTabCounts {
  int nexus_count = 0;
  int total_count = 0;

  friend bool operator==(const SessionTabCounts&,
                         const SessionTabCounts&) = default;
};

// One tagged tab, reduced to the two facts the counts care about. The
// browser-walking caller produces these; keeping the input this narrow is
// what lets the fold + diff below stay pure (//base-only) and land in the
// `unit_tests_lite` binary.
struct SessionTabSnapshotEntry {
  std::string session_key;
  bool in_nexus_window = false;
};

using SessionTabCountsMap = base::flat_map<std::string, SessionTabCounts>;

// Folds a walk's snapshot entries into per-session counts.
SessionTabCountsMap BuildCountsFromSnapshot(
    const std::vector<SessionTabSnapshotEntry>& snapshot);

// A session whose counts changed between two recomputes. Sessions present
// in `old_counts` but absent from `new_counts` are reported with zeroed
// counts so observers see the N→0 transition (last tab closed).
struct SessionTabCountsChange {
  std::string session_key;
  SessionTabCounts counts;
};

std::vector<SessionTabCountsChange> DiffSessionTabCounts(
    const SessionTabCountsMap& old_counts,
    const SessionTabCountsMap& new_counts);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_SESSION_NEXUS_SESSION_TAB_COUNTS_H_
