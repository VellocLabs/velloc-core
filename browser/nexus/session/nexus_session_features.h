// Copyright 2026 The Custom Browser Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_SESSION_NEXUS_SESSION_FEATURES_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_SESSION_NEXUS_SESSION_FEATURES_H_

#include "base/feature_list.h"

namespace nexus::features {

// When enabled, every Nexus session owns a Chromium tab group; tabs
// created by that session's BrowserTool are tagged with the session_key
// (NexusSessionTabTag) and added to the group, tab-targeting actions
// reject cross-session tab_ids, observations filter to the session's
// own tabs, and OnSessionDeleted closes the session's tabs.
//
// When disabled, the legacy single-pool behavior is preserved:
// RunCreateTabAction does not tag or group; ResolveTabIdOrReject /
// GetActiveTabHandleId fall back to "active tab in active window";
// BuildObservationDict does not filter.
BASE_DECLARE_FEATURE(kSessionScopedTabs);

bool IsSessionScopedTabsEnabled();

}  // namespace nexus::features

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_SESSION_NEXUS_SESSION_FEATURES_H_
