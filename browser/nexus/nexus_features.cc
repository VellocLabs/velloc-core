// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/nexus_features.h"

namespace nexus::features {

BASE_FEATURE(kPrereleaseNexusProcessCapability,
             "PrereleaseNexusProcessCapability",
             base::FEATURE_DISABLED_BY_DEFAULT);

bool IsNexusProcessCapabilityEnabled() {
  return base::FeatureList::IsEnabled(kPrereleaseNexusProcessCapability);
}

}  // namespace nexus::features
