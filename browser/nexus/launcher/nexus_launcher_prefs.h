// Copyright 2026 The Custom Browser Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_LAUNCHER_NEXUS_LAUNCHER_PREFS_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_LAUNCHER_NEXUS_LAUNCHER_PREFS_H_

class PrefRegistrySimple;

namespace nexus {
namespace prefs {

// local_state pref (per-installation, browser-wide). When true, the Nexus
// launcher holds a keep-alive so the browser process survives after the last
// window closes — keeping the Nexus surface reachable while the browser is
// minimized/closed (a later stage surfaces a system-tray icon + global hotkey
// that re-open the Nexus window on demand).
//
// This is the Nexus analogue of Chrome's glic.launcher.enabled. It is distinct
// from prefs::kBackgroundModeEnabled ("continue running background apps"),
// which only keeps the process alive when a background-capable extension is
// installed. Default false. Read by NexusBackgroundModeManager; the System
// settings page writes it via NexusSettingsHandler (added in a later stage).
inline constexpr char kNexusLauncherEnabled[] = "nexus.launcher_enabled";

}  // namespace prefs

// Registers the Nexus launcher's local_state prefs. Called from
// chrome/browser/prefs/browser_prefs.cc RegisterLocalState(), guarded by
// BUILDFLAG(ENABLE_CUSTOM_BROWSER), alongside glic::prefs::RegisterLocalState.
void RegisterNexusLauncherLocalStatePrefs(PrefRegistrySimple* registry);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_LAUNCHER_NEXUS_LAUNCHER_PREFS_H_
