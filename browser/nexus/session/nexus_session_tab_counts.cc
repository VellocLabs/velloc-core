// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/session/nexus_session_tab_counts.h"

namespace nexus {

SessionTabCountsMap BuildCountsFromSnapshot(
    const std::vector<SessionTabSnapshotEntry>& snapshot) {
  SessionTabCountsMap counts;
  for (const SessionTabSnapshotEntry& entry : snapshot) {
    SessionTabCounts& c = counts[entry.session_key];
    ++c.total_count;
    if (entry.in_nexus_window) {
      ++c.nexus_count;
    }
  }
  return counts;
}

std::vector<SessionTabCountsChange> DiffSessionTabCounts(
    const SessionTabCountsMap& old_counts,
    const SessionTabCountsMap& new_counts) {
  std::vector<SessionTabCountsChange> changes;
  for (const auto& [key, counts] : new_counts) {
    auto it = old_counts.find(key);
    if (it == old_counts.end() || !(it->second == counts)) {
      changes.push_back({key, counts});
    }
  }
  for (const auto& [key, counts] : old_counts) {
    if (!new_counts.contains(key)) {
      changes.push_back({key, SessionTabCounts{}});
    }
  }
  return changes;
}

}  // namespace nexus
