// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/ui/view/nexus_quit_confirmation.h"

#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/ui/simple_message_box.h"
#include "custom_browser/browser/nexus/remote_access/nexus_remote_access_clients.h"
#include "custom_browser/common/branding_constants.h"
#include "custom_browser/resources/grit/custom_browser_strings.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/gfx/native_ui_types.h"

namespace custom_browser {

namespace {

bool g_dialog_showing = false;

void OnAnswered(base::OnceClosure quit, chrome::MessageBoxResult result) {
  g_dialog_showing = false;
  if (result == chrome::MESSAGE_BOX_RESULT_YES) {
    std::move(quit).Run();
  }
}

}  // namespace

void ConfirmQuitWithRemoteDevices(base::OnceClosure quit) {
  const int devices = nexus::remote_access::GetConnectedClients().clients;
  if (devices <= 0) {
    std::move(quit).Run();
    return;
  }
  if (g_dialog_showing) {
    return;
  }
  g_dialog_showing = true;
  const std::u16string product =
      base::ASCIIToUTF16(kCustomBrowserProductName);
  // No parent: the tray has no window, and a window-modal box on the menu's
  // window would block only that window while the question is app-wide.
  chrome::ShowQuestionMessageBoxAsync(
      gfx::NativeWindow(),
      l10n_util::GetStringFUTF16(IDS_NEXUS_QUIT_REMOTE_DEVICES_TITLE, product),
      l10n_util::GetPluralStringFUTF16(IDS_NEXUS_QUIT_REMOTE_DEVICES_BODY,
                                       devices),
      base::BindOnce(&OnAnswered, std::move(quit)));
}

}  // namespace custom_browser
