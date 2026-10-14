// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_LAUNCHER_NEXUS_LAUNCHER_DELEGATE_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_LAUNCHER_NEXUS_LAUNCHER_DELEGATE_H_

#include "base/functional/callback_forward.h"

class Profile;

namespace nexus {

// Bridges the launcher's tray icon to the window-opening logic that lives in
// the UI layer (custom_browser::window_util, compiled into //chrome/browser/ui).
//
// The launcher target deliberately does NOT depend on //chrome/browser/ui: that
// target transitively depends back on the launcher (via the chromium_hooks
// integration), so a direct dependency would form a build cycle. Instead the UI
// layer implements this interface and registers it at startup; the tray icon
// calls it through SetNexusLauncherDelegate/GetNexusLauncherDelegate.
class NexusLauncherDelegate {
 public:
  virtual ~NexusLauncherDelegate() = default;

  // Focuses the user's existing Nexus window, or opens one if none exists
  // (works even when no browser window is currently open, because the launcher
  // keep-alive keeps the process running).
  virtual void OpenOrFocusNexusWindow() = 0;

  // The same for `profile`'s Nexus window - for a caller that acts for one
  // profile (an alarm that fired in it) rather than for the user at large.
  // A null `profile` means the launch profile, as OpenOrFocusNexusWindow.
  virtual void OpenOrFocusNexusWindowForProfile(Profile* profile) = 0;

  // Focuses an existing normal (non-Nexus) browser window, or opens one. Backs
  // the tray menu's "Open Browser Window".
  virtual void OpenOrFocusNormalWindow() = 0;

  // Asked before the tray menu's "Quit" does anything: runs `proceed` at once
  // when no remote-access device is connected, otherwise after the user
  // confirms that quitting disconnects them — and drops it if they decline.
  // The manager keeps its keep-alive until `proceed` runs, so declining leaves
  // the process exactly as it was.
  virtual void ConfirmQuit(base::OnceClosure proceed) = 0;

  // Closes all browser windows (graceful quit). Backs the tray menu's "Quit".
  // The manager releases its keep-alive before calling this, so the process
  // exits once the windows are gone.
  virtual void Quit() = 0;
};

// Sets/returns the process-global delegate. The UI layer owns the instance and
// must outlive the launcher; passing nullptr clears it. Get returns nullptr if
// the UI layer has not registered one yet (e.g. very early startup), in which
// case the tray click is a no-op.
void SetNexusLauncherDelegate(NexusLauncherDelegate* delegate);
NexusLauncherDelegate* GetNexusLauncherDelegate();

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_LAUNCHER_NEXUS_LAUNCHER_DELEGATE_H_
