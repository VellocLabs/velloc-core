// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/session/nexus_session_features.h"

namespace nexus::features {

BASE_FEATURE(kSessionScopedTabs,
             "NexusSessionScopedTabs",
             base::FEATURE_ENABLED_BY_DEFAULT);

bool IsSessionScopedTabsEnabled() {
  return base::FeatureList::IsEnabled(kSessionScopedTabs);
}

}  // namespace nexus::features
