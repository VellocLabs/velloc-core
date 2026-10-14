// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/launcher/nexus_launcher_prefs.h"

#include "components/prefs/pref_registry_simple.h"

namespace nexus {

void RegisterNexusLauncherLocalStatePrefs(PrefRegistrySimple* registry) {
  registry->RegisterBooleanPref(prefs::kNexusLauncherEnabled, false);
}

}  // namespace nexus
