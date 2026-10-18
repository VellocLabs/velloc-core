// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/launcher/nexus_status_icon.h"

#include <memory>
#include <utility>

#include "base/functional/callback.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/status_icons/status_icon.h"
#include "chrome/browser/status_icons/status_icon_menu_model.h"
#include "chrome/browser/status_icons/status_tray.h"
#include "chrome/grit/theme_resources.h"
#include "custom_browser/browser/nexus/launcher/nexus_launcher_delegate.h"
#include "custom_browser/common/branding_constants.h"
#include "custom_browser/resources/grit/custom_browser_strings.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/models/menu_separator_types.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/gfx/image/image_skia.h"

namespace nexus {

namespace {

// Context-menu command ids (arbitrary, local to this menu).
enum NexusTrayCommand {
  kOpenNexusWindow = 1,
  kQuit,
};

}  // namespace

NexusStatusIcon::NexusStatusIcon(StatusTray* status_tray,
                                 base::RepeatingClosure quit_callback)
    : status_tray_(status_tray), quit_callback_(std::move(quit_callback)) {
  status_icon_ = status_tray_->CreateStatusIcon(
      StatusTray::OTHER_ICON, GetProductLogoIcon(),
      base::ASCIIToUTF16(custom_browser::kCustomBrowserProductName));
  if (!status_icon_) {
    return;
  }
  status_icon_->AddObserver(this);
  // The tooltip follows remote access for as long as the icon lives.
  remote_access::AddConnectedClientsObserver(this);
  UpdateToolTip(remote_access::GetConnectedClients());

  auto menu = std::make_unique<StatusIconMenuModel>(this);
  menu->AddItem(NexusTrayCommand::kOpenNexusWindow,
                l10n_util::GetStringUTF16(IDS_NEXUS_TRAY_SHOW_APP));
  menu->AddSeparator(ui::NORMAL_SEPARATOR);
  menu->AddItem(NexusTrayCommand::kQuit,
                l10n_util::GetStringUTF16(IDS_NEXUS_TRAY_QUIT));
  status_icon_->SetContextMenu(std::move(menu));
}

NexusStatusIcon::~NexusStatusIcon() {
  remote_access::RemoveConnectedClientsObserver(this);
  if (status_icon_) {
    status_icon_->RemoveObserver(this);
    status_tray_->RemoveStatusIcon(status_icon_);
    status_icon_ = nullptr;
  }
}

void NexusStatusIcon::OnStatusIconClicked() {
  OpenNexusWindow();
}

void NexusStatusIcon::ExecuteCommand(int command_id, int event_flags) {
  switch (command_id) {
    case NexusTrayCommand::kOpenNexusWindow:
      OpenNexusWindow();
      break;
    case NexusTrayCommand::kQuit:
      if (quit_callback_) {
        quit_callback_.Run();
      }
      break;
    default:
      break;
  }
}

void NexusStatusIcon::OnConnectedClientsChanged(
    const remote_access::ConnectedClients& summary) {
  UpdateToolTip(summary);
}

void NexusStatusIcon::UpdateToolTip(
    const remote_access::ConnectedClients& summary) {
  if (!status_icon_) {
    return;
  }
  std::u16string tool_tip =
      base::ASCIIToUTF16(custom_browser::kCustomBrowserProductName);
  if (remote_access::HasConnectedDevices(summary)) {
    tool_tip += u'\n';
    tool_tip += l10n_util::GetPluralStringFUTF16(
        IDS_NEXUS_TRAY_REMOTE_ACCESS_DEVICES, summary.clients);
  }
  status_icon_->SetToolTip(tool_tip);
}

void NexusStatusIcon::OpenNexusWindow() {
  // The UI layer registers the delegate at startup; it may be null only during
  // a brief early-startup window, in which case the action is a no-op.
  if (NexusLauncherDelegate* delegate = GetNexusLauncherDelegate()) {
    delegate->OpenOrFocusNexusWindow();
  }
}

// static
gfx::ImageSkia NexusStatusIcon::GetProductLogoIcon() {
  // IDR_PRODUCT_LOGO_16 resolves to ${branding_path_component}/product_logo_16
  // — i.e. Velloc's production logo PNG in a branded build (theme_resources.grd).
  return *ui::ResourceBundle::GetSharedInstance().GetImageSkiaNamed(
      IDR_PRODUCT_LOGO_16);
}

}  // namespace nexus
