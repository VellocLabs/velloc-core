// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/web_window/sign_in_window.h"

#include "base/strings/utf_string_conversions.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/web_contents.h"
#include "custom_browser/common/branding_constants.h"
#include "custom_browser/resources/grit/custom_browser_strings.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/page_transition_types.h"

namespace nexus {

SignInStatusText LocalizedSignInStatusText() {
  SignInStatusText text;
  text.finishing = l10n_util::GetStringUTF16(IDS_SIGN_IN_STATUS_FINISHING);
  text.succeeded = l10n_util::GetStringUTF16(IDS_SIGN_IN_STATUS_SUCCEEDED);
  text.closing = l10n_util::GetStringUTF16(IDS_SIGN_IN_STATUS_CLOSING);
  text.failed = l10n_util::GetStringUTF16(IDS_SIGN_IN_STATUS_FAILED);
  text.failed_hint = l10n_util::GetStringFUTF16(
      IDS_SIGN_IN_STATUS_FAILED_HINT,
      base::ASCIIToUTF16(custom_browser::kCustomBrowserProductName));
  return text;
}

std::u16string SignInWindowTitle(std::u16string_view service_name) {
  return l10n_util::GetStringFUTF16(IDS_SIGN_IN_WINDOW_TITLE,
                                    std::u16string(service_name));
}

void ShowSignInStatus(content::WebContents* contents,
                      SignInStatus status,
                      std::u16string_view reason) {
  if (!contents) {
    return;
  }
  content::NavigationController::LoadURLParams load(
      MakeSignInStatusPage(status, LocalizedSignInStatusText(), reason));
  load.transition_type = ui::PAGE_TRANSITION_AUTO_TOPLEVEL;
  load.should_replace_current_entry = true;
  contents->GetController().LoadURLWithParams(load);
}

}  // namespace nexus
