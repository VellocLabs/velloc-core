// Copyright 2026 The Custom Browser Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/launcher/nexus_background_mode_manager.h"

#include <memory>

#include "base/check.h"
#include "base/functional/bind.h"
#include "chrome/browser/browser_process.h"
#include "chrome/common/pref_names.h"
#include "components/keep_alive_registry/keep_alive_registry.h"
#include "components/keep_alive_registry/keep_alive_types.h"
#include "components/keep_alive_registry/scoped_keep_alive.h"
#include "components/prefs/pref_service.h"
#include "custom_browser/browser/nexus/launcher/nexus_launcher_delegate.h"
#include "custom_browser/browser/nexus/launcher/nexus_launcher_prefs.h"
#include "custom_browser/browser/nexus/launcher/nexus_status_icon.h"

namespace nexus {

NexusBackgroundModeManager::NexusBackgroundModeManager(StatusTray* status_tray)
    : status_tray_(status_tray) {
  PrefService* local_state = g_browser_process->local_state();
  CHECK(local_state);
  MigrateLegacyLauncherPref(local_state);
  local_state_registrar_.Init(local_state);
  local_state_registrar_.Add(
      ::prefs::kBackgroundModeEnabled,
      base::BindRepeating(&NexusBackgroundModeManager::OnEnabledPrefChanged,
                          base::Unretained(this)));
  UpdateState();
}

// static
void NexusBackgroundModeManager::MigrateLegacyLauncherPref(
    PrefService* local_state) {
  if (!local_state || !local_state->HasPrefPath(prefs::kNexusLauncherEnabled)) {
    return;
  }
  // The user's explicit choice on the old tray toggle carries over, unless
  // they already touched the new toggle (then their newer choice wins).
  // GetUserPrefValue, not HasPrefPath: a policy-managed value must not read
  // as "the user chose this" (it also cannot be overwritten below).
  if (!local_state->GetUserPrefValue(::prefs::kBackgroundModeEnabled)) {
    local_state->SetBoolean(
        ::prefs::kBackgroundModeEnabled,
        local_state->GetBoolean(prefs::kNexusLauncherEnabled));
  }
  local_state->ClearPref(prefs::kNexusLauncherEnabled);
}

NexusBackgroundModeManager::~NexusBackgroundModeManager() = default;

void NexusBackgroundModeManager::OnEnabledPrefChanged() {
  UpdateState();
}

void NexusBackgroundModeManager::UpdateState() {
  PrefService* local_state = g_browser_process->local_state();
  const bool enabled =
      local_state && local_state->GetBoolean(::prefs::kBackgroundModeEnabled);

  // Keep-alive: hold the process open while enabled.
  if (enabled && !keep_alive_) {
    // Never register a keep-alive during shutdown: KeepAliveRegistry CHECKs.
    if (!KeepAliveRegistry::GetInstance()->IsShuttingDown()) {
      keep_alive_ = std::make_unique<ScopedKeepAlive>(
          KeepAliveOrigin::NEXUS_LAUNCHER, KeepAliveRestartOption::ENABLED);
    }
  } else if (!enabled && keep_alive_) {
    keep_alive_.reset();
  }

  // Tray icon: only when a status tray is available (absent in unit tests).
  if (enabled && status_tray_ && !status_icon_) {
    status_icon_ = std::make_unique<NexusStatusIcon>(
        status_tray_,
        base::BindRepeating(&NexusBackgroundModeManager::Quit,
                            base::Unretained(this)));
  } else if (!enabled && status_icon_) {
    status_icon_.reset();
  }
}

void NexusBackgroundModeManager::Quit() {
  // Drop our keep-alive so the launcher no longer holds the process open, then
  // ask the UI layer to close all browser windows. chrome::CloseAllBrowsers
  // lives in //chrome/browser/ui, which the launcher target can't depend on
  // (cycle), so it goes through the delegate. The closure is owned by the tray
  // icon (a member), so `this` is always valid when it runs.
  keep_alive_.reset();
  if (NexusLauncherDelegate* delegate = GetNexusLauncherDelegate()) {
    delegate->Quit();
  }
}

NexusLauncherLifecycleHook::NexusLauncherLifecycleHook() = default;
NexusLauncherLifecycleHook::~NexusLauncherLifecycleHook() = default;

void NexusLauncherLifecycleHook::PreMainMessageLoopRun() {
  manager_ = std::make_unique<NexusBackgroundModeManager>(
      g_browser_process->status_tray());
}

void NexusLauncherLifecycleHook::PostMainMessageLoopRun() {
  manager_.reset();
}

}  // namespace nexus
