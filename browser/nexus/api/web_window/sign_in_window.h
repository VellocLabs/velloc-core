// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_WEB_WINDOW_SIGN_IN_WINDOW_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_WEB_WINDOW_SIGN_IN_WINDOW_H_

#include <string>
#include <string_view>

#include "custom_browser/browser/nexus/api/web_window/sign_in_status_page.h"

namespace content {
class WebContents;
}  // namespace content

namespace nexus {

// The status-page text in the browser's UI language.
SignInStatusText LocalizedSignInStatusText();

// "Sign in to {service_name}", the fixed title of a sign-in window.
std::u16string SignInWindowTitle(std::u16string_view service_name);

// Replaces the page in `contents` with the host's status page for `status`
// (F-29). Contract: UI thread; null `contents` is a no-op. The load is
// browser-initiated and replaces the current history entry, so Back does
// not return to the status page.
void ShowSignInStatus(content::WebContents* contents,
                      SignInStatus status,
                      std::u16string_view reason = {});

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_WEB_WINDOW_SIGN_IN_WINDOW_H_
