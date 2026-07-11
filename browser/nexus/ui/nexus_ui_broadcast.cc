// Copyright 2025 The Custom Browser Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/ui/nexus_ui_broadcast.h"

#include <string>
#include <utility>

#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "custom_browser/ui/view/custom_browser_view.h"
#include "custom_browser/ui/web_ui/custom_browser_webui_view.h"

namespace nexus {

namespace {

// Walks every normal browser window for `profile`, finds the
// `CustomBrowserWebuiView`, and invokes `callback` with its live `WebUI*`.
// Skips windows that don't belong to `profile`, aren't normal browsers, or
// whose WebUI is not yet ready to accept JS calls.
template <typename F>
void ForEachLiveWebUiInProfile(Profile* profile, F callback) {
  if (!profile) {
    return;
  }

  ForEachCurrentBrowserWindowInterfaceOrderedByActivation(
      [&](BrowserWindowInterface* window) {
        Browser* browser =
            window ? window->GetBrowserForMigrationOnly() : nullptr;
        if (!browser || browser->profile() != profile ||
            !browser->is_type_normal()) {
          return true;
        }

        auto* browser_view = BrowserView::GetBrowserViewForBrowser(browser);
        if (!browser_view) {
          return true;
        }

        auto* custom_browser_view = static_cast<CustomBrowserView*>(browser_view);
        auto* webui_view =
            custom_browser_view->views().custom_browser_webui_view.get();
        content::WebContents* web_contents =
            webui_view ? webui_view->GetWebContents() : nullptr;
        content::WebUI* web_ui =
            web_contents ? web_contents->GetWebUI() : nullptr;
        if (!web_ui || !web_ui->CanCallJavascript()) {
          return true;
        }

        callback(web_ui);
        return true;
      });
}

}  // namespace

void BroadcastWebUiListenerEventToProfile(Profile* profile,
                                          std::string_view event_name) {
  ForEachLiveWebUiInProfile(profile, [&](content::WebUI* web_ui) {
    web_ui->CallJavascriptFunctionUnsafe(
        "cr.webUIListenerCallback",
        base::Value(std::string(event_name)));
  });
}

void BroadcastWebUiListenerEventToProfile(Profile* profile,
                                          std::string_view event_name,
                                          base::Value payload) {
  // `CallJavascriptFunctionUnsafe` doesn't expose a vector overload that
  // takes ownership of values, so each window receives its own clone.
  ForEachLiveWebUiInProfile(profile, [&](content::WebUI* web_ui) {
    web_ui->CallJavascriptFunctionUnsafe(
        "cr.webUIListenerCallback",
        base::Value(std::string(event_name)),
        payload.Clone());
  });
}

}  // namespace nexus
