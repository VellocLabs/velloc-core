// Copyright 2026 The Custom Browser Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_LAUNCHER_NEXUS_BACKGROUND_MODE_MANAGER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_LAUNCHER_NEXUS_BACKGROUND_MODE_MANAGER_H_

#include <memory>

#include "base/callback_list.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "components/prefs/pref_change_registrar.h"
#include "custom_browser/chromium_hooks/hook_types.h"

class PrefService;
class ScopedKeepAlive;
class StatusTray;

namespace nexus {

class NexusStatusIcon;

// Browser-process-global controller for the Nexus launcher's background mode.
//
// Mirrors the relevant slice of Chrome's glic::GlicBackgroundModeManager: it
// observes the per-installation local_state pref
// prefs::kBackgroundModeEnabled — the ONE System toggle ("Keep Velloc running
// in the background", default on) — and, while that pref is true, both
//   * holds a ScopedKeepAlive (KeepAliveOrigin::NEXUS_LAUNCHER) so the browser
//     process survives after the last window closes, and
//   * shows a system-tray icon (Velloc's production logo) whose click re-opens
//     the Nexus window.
// The same pref makes the kNexus window park its page instead of destroying
// it on close (nexus::NexusBackgroundPanelHost), so one switch means: close
// the window and Velloc keeps running — sessions included — in the tray.
//
// prefs::kBackgroundModeEnabled is Chrome's "continue running background
// apps" pref; Chrome's own BackgroundModeManager only acts on it when a
// background-capable extension is installed, which Velloc never has, so
// reusing it is safe and gives the toggle a single source of truth. The
// former nexus::prefs::kNexusLauncherEnabled is migrated into it once
// (MigrateLegacyLauncherPref) and otherwise no longer read.
//
// A real quit still quits. Like Chrome's BackgroundModeManager
// (OnAppTerminating), the manager listens for APP_TERMINATING
// (browser_shutdown::AddAppTerminatingCallback), which fires when the last
// browser closes during a try-to-quit exit: CDP Browser.close / end-session /
// restart (chrome::ExitIgnoreUnloadHandlers, chrome::AttemptExit). It then drops
// the keep-alive and the tray icon for good. A plain app-menu Exit
// (chrome::AttemptUserExit, try_to_quit=false) does not fire it, so that one
// still leaves Velloc in the tray, as upstream background mode does.
class NexusBackgroundModeManager {
 public:
  // `status_tray` may be null (e.g. in unit tests / headless), in which case
  // the keep-alive is still managed but no tray icon is created.
  explicit NexusBackgroundModeManager(StatusTray* status_tray);
  ~NexusBackgroundModeManager();

  NexusBackgroundModeManager(const NexusBackgroundModeManager&) = delete;
  NexusBackgroundModeManager& operator=(const NexusBackgroundModeManager&) =
      delete;

  // One-time migration of the pre-2026-09-10 local_state pref
  // nexus::prefs::kNexusLauncherEnabled (the old "Show in the system tray"
  // toggle, default false) into prefs::kBackgroundModeEnabled: a user-set
  // legacy value is copied over unless the user has since set the new pref
  // themselves, and the legacy pref is cleared either way. Idempotent; a
  // no-op when the legacy pref was never set. Public for tests.
  static void MigrateLegacyLauncherPref(PrefService* local_state);

  // True while the keep-alive is held (i.e. the background pref is enabled).
  bool IsKeepingAliveForTesting() const { return keep_alive_ != nullptr; }
  NexusStatusIcon* status_icon_for_testing() const {
    return status_icon_.get();
  }

  // Tray-menu "Quit". Asks the UI-layer delegate to confirm first (it only
  // asks the user while remote-access devices are connected), and only on
  // confirmation releases the keep-alive and closes all browsers so the
  // process can exit. Public for tests.
  void Quit();

 private:
  void OnEnabledPrefChanged();

  // APP_TERMINATING: releases the keep-alive and the tray icon, and stops
  // UpdateState() from acquiring them again.
  void OnAppTerminating();

  // Acquires/releases the keep-alive and the tray icon to match the pref.
  // A no-op once the app is terminating.
  void UpdateState();

  // The confirmed half of Quit(): releases the keep-alive, then asks the
  // UI-layer delegate to close all browsers.
  void QuitConfirmed();

  PrefChangeRegistrar local_state_registrar_;
  raw_ptr<StatusTray> status_tray_;
  std::unique_ptr<ScopedKeepAlive> keep_alive_;
  std::unique_ptr<NexusStatusIcon> status_icon_;
  base::CallbackListSubscription on_app_terminating_subscription_;
  bool app_terminating_ = false;

  // A confirmation can be answered after shutdown began and the manager went.
  base::WeakPtrFactory<NexusBackgroundModeManager> weak_factory_{this};
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
