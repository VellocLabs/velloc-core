// Copyright 2026 The Custom Browser Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_LAUNCHER_NEXUS_BACKGROUND_MODE_MANAGER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_LAUNCHER_NEXUS_BACKGROUND_MODE_MANAGER_H_

#include <memory>

#include "base/memory/raw_ptr.h"
#include "components/prefs/pref_change_registrar.h"
#include "custom_browser/chromium_hooks/hook_types.h"

class ScopedKeepAlive;
class StatusTray;

namespace nexus {

class NexusStatusIcon;

// Browser-process-global controller for the Nexus launcher's background mode.
//
// Mirrors the relevant slice of Chrome's glic::GlicBackgroundModeManager: it
// observes the per-installation local_state pref
// nexus::prefs::kNexusLauncherEnabled and, while that pref is true, both
//   * holds a ScopedKeepAlive (KeepAliveOrigin::NEXUS_LAUNCHER) so the browser
//     process survives after the last window closes, and
//   * shows a system-tray icon (Velloc's production logo) whose click re-opens
//     the Nexus window.
//
// This is deliberately independent of Chrome's BackgroundModeManager
// (prefs::kBackgroundModeEnabled), which only keeps the process alive when a
// background-capable extension is installed.
class NexusBackgroundModeManager {
 public:
  // `status_tray` may be null (e.g. in unit tests / headless), in which case
  // the keep-alive is still managed but no tray icon is created.
  explicit NexusBackgroundModeManager(StatusTray* status_tray);
  ~NexusBackgroundModeManager();

  NexusBackgroundModeManager(const NexusBackgroundModeManager&) = delete;
  NexusBackgroundModeManager& operator=(const NexusBackgroundModeManager&) =
      delete;

  // True while the keep-alive is held (i.e. the launcher pref is enabled).
  bool IsKeepingAliveForTesting() const { return keep_alive_ != nullptr; }
  NexusStatusIcon* status_icon_for_testing() const {
    return status_icon_.get();
  }

 private:
  void OnEnabledPrefChanged();

  // Acquires/releases the keep-alive and the tray icon to match the pref.
  void UpdateState();

  // Tray-menu "Quit": releases the keep-alive, then asks the UI-layer delegate
  // to close all browsers so the process can exit.
  void Quit();

  PrefChangeRegistrar local_state_registrar_;
  raw_ptr<StatusTray> status_tray_;
  std::unique_ptr<ScopedKeepAlive> keep_alive_;
  std::unique_ptr<NexusStatusIcon> status_icon_;
};

// Owns the NexusBackgroundModeManager for the browser-process lifetime.
// Registered with the HookRegistrar from RegisterAllHooks(). The manager is
// created in PreMainMessageLoopRun (local_state + the status tray exist by
// then), destroyed in PostMainMessageLoopRun.
class NexusLauncherLifecycleHook
    : public custom_browser::BrowserMainLifecycleHook {
 public:
  NexusLauncherLifecycleHook();
  ~NexusLauncherLifecycleHook() override;

  // custom_browser::BrowserMainLifecycleHook:
  void PreMainMessageLoopRun() override;
  void PostMainMessageLoopRun() override;

 private:
  std::unique_ptr<NexusBackgroundModeManager> manager_;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_LAUNCHER_NEXUS_BACKGROUND_MODE_MANAGER_H_
