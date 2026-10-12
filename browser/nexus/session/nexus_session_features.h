// Copyright 2026 The Custom Browser Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_SESSION_NEXUS_SESSION_FEATURES_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_SESSION_NEXUS_SESSION_FEATURES_H_

#include "base/feature_list.h"

namespace nexus::features {

// When enabled, each Nexus session owns a tab group: its BrowserTool tags and
// groups the tabs it creates, rejects cross-session tab_ids, filters
// observations to its own tabs, and closes them on OnSessionDeleted. When
// disabled, tools act on the active tab of the active window, unfiltered.
BASE_DECLARE_FEATURE(kSessionScopedTabs);

bool IsSessionScopedTabsEnabled();

}  // namespace nexus::features

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_SESSION_NEXUS_SESSION_FEATURES_H_
