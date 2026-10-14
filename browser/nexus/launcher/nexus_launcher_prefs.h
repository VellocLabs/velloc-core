// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_LAUNCHER_NEXUS_LAUNCHER_PREFS_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_LAUNCHER_NEXUS_LAUNCHER_PREFS_H_

class PrefRegistrySimple;

namespace nexus {
namespace prefs {

// LEGACY local_state pref (per-installation, browser-wide), superseded
// by prefs::kBackgroundModeEnabled — the single System toggle that
// now drives the tray keep-alive AND the background panel. Still registered
// (default false) only so NexusBackgroundModeManager::MigrateLegacyLauncherPref
// can read a user's old "Show in the system tray" choice once and clear it.
// Nothing else reads or writes it; the systemPrefs bridge's `launcherEnabled`
// / `setLauncher` and the panel's get/setNexusLauncherEnabled are aliases of
// the background pref for wire compatibility.
inline constexpr char kNexusLauncherEnabled[] = "nexus.launcher_enabled";

}  // namespace prefs

// Registers the Nexus launcher's local_state prefs. Called from
// chrome/browser/prefs/browser_prefs.cc RegisterLocalState(), guarded by
// BUILDFLAG(ENABLE_CUSTOM_BROWSER), alongside glic::prefs::RegisterLocalState.
void RegisterNexusLauncherLocalStatePrefs(PrefRegistrySimple* registry);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_LAUNCHER_NEXUS_LAUNCHER_PREFS_H_
