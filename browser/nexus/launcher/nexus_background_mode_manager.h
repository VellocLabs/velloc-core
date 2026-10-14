// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

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

// Browser-process-global background mode. While local_state
// prefs::kBackgroundModeEnabled is true it holds a ScopedKeepAlive and shows a
// tray icon that re-opens the Nexus window; the same pref makes a closed kNexus
// window park its page. APP_TERMINATING (a try-to-quit exit) drops both for
// good; a plain app-menu Exit leaves Velloc in the tray.
class NexusBackgroundModeManager {
 public:
  // `status_tray` may be null (e.g. in unit tests / headless), in which case
  // the keep-alive is still managed but no tray icon is created.
  explicit NexusBackgroundModeManager(StatusTray* status_tray);
  ~NexusBackgroundModeManager();

  NexusBackgroundModeManager(const NexusBackgroundModeManager&) = delete;
  NexusBackgroundModeManager& operator=(const NexusBackgroundModeManager&) =
      delete;

  // One-time migration of the legacy local_state pref
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
