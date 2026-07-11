// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_FEATURES_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_FEATURES_H_

#include "base/feature_list.h"

namespace nexus::features {

// Gate for the prerelease `process` capability (Mojo
// `nexus.mojom.NexusShellService` + `process` bridge namespace). When
// disabled, `NexusBridgePortService` does not register the `process`
// capability so the guest's `requireCapability('process')` rejects with
// `PROCESS_UNAVAILABLE`. The native shell-resolver service is also not
// launched, so no host PATH probing happens for users who don't opt in.
//
// Off by default — flipping to `FEATURE_ENABLED_BY_DEFAULT` is the
// release-graduation step (see `release-feature-flag` skill).
BASE_DECLARE_FEATURE(kPrereleaseNexusProcessCapability);

// Returns true iff the process capability is currently enabled. Helper
// for non-base callers that do not want to take a dependency on
// `base/feature_list.h` directly. Cheap (atomic load).
bool IsNexusProcessCapabilityEnabled();

}  // namespace nexus::features

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_FEATURES_H_
