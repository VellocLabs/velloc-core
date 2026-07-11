// Copyright 2026 The Custom Browser Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_LAUNCHER_NEXUS_STATUS_ICON_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_LAUNCHER_NEXUS_STATUS_ICON_H_

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "chrome/browser/status_icons/status_icon_menu_model.h"
#include "chrome/browser/status_icons/status_icon_observer.h"

namespace gfx {
class ImageSkia;
}

class StatusIcon;
class StatusTray;

namespace nexus {

// System-tray icon for the Nexus launcher. Shows Velloc's production logo
// (IDR_PRODUCT_LOGO_*) with a context menu: "Open Nexus Window", "Open Browser
// Window", and "Quit". The window actions route through the registered
// NexusLauncherDelegate; "Quit" runs the supplied callback (the manager releases
// its keep-alive and asks the delegate to close all browsers). A left-click
// opens the Nexus window. Owned by NexusBackgroundModeManager and only alive
// while the launcher is enabled.
class NexusStatusIcon : public StatusIconObserver,
                        public StatusIconMenuModel::Delegate {
 public:
  // `quit_callback` is run when the user picks "Quit".
  NexusStatusIcon(StatusTray* status_tray, base::RepeatingClosure quit_callback);
  ~NexusStatusIcon() override;

  NexusStatusIcon(const NexusStatusIcon&) = delete;
  NexusStatusIcon& operator=(const NexusStatusIcon&) = delete;

  // StatusIconObserver:
  void OnStatusIconClicked() override;

  // StatusIconMenuModel::Delegate:
  void ExecuteCommand(int command_id, int event_flags) override;

 private:
  // Opens/focuses the Nexus window via the registered delegate (no-op if none).
  void OpenNexusWindow();

  // Velloc's production logo as a tray-sized image, from the resource bundle.
  static gfx::ImageSkia GetProductLogoIcon();

  raw_ptr<StatusTray> status_tray_;
  raw_ptr<StatusIcon> status_icon_ = nullptr;
  base::RepeatingClosure quit_callback_;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_LAUNCHER_NEXUS_STATUS_ICON_H_
