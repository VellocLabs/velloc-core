// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/web_window/single_web_window.h"

#include <utility>

#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/create_browser_window.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/site_instance.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/referrer.h"
#include "custom_browser/ui/view/custom_browser_window_util.h"
#include "ui/base/page_transition_types.h"
#include "url/gurl.h"

namespace nexus {

SingleWebWindowParams::SingleWebWindowParams() = default;
SingleWebWindowParams::SingleWebWindowParams(const SingleWebWindowParams&) =
    default;
SingleWebWindowParams& SingleWebWindowParams::operator=(
    const SingleWebWindowParams&) = default;
SingleWebWindowParams::~SingleWebWindowParams() = default;

BrowserWindowInterface* OpenSingleWebWindow(
    Profile* profile,
    const GURL& url,
    const SingleWebWindowParams& params) {
  if (!profile || GetBrowserWindowCreationStatusForProfile(*profile) !=
                      BrowserWindowInterface::CreationStatus::kOk) {
    return nullptr;
  }

  BrowserWindowCreateParams create(BrowserWindowInterface::TYPE_NORMAL,
                                   profile, /*from_user_gesture=*/true);
  create.omit_from_session_restore = true;
  create.should_trigger_session_restore = false;
  create.initial_show_state = params.show_state;
  if (params.bounds) {
    create.initial_bounds = *params.bounds;
    create.initial_origin_specified =
        params.origin_specified
            ? BrowserWindowCreateParams::ValueSpecified::kSpecified
            : BrowserWindowCreateParams::ValueSpecified::kUnspecified;
  }
  // The marker is consumed by the CustomBrowserView inside this synchronous
  // Create; the second consume clears it should no view have taken it.
  custom_browser::window_util::SetPendingSingleWebWindow(profile,
                                                         params.title);
  BrowserWindowInterface* browser = CreateBrowserWindow(std::move(create));
  custom_browser::window_util::ConsumePendingSingleWebWindow(profile);
  if (!browser || !custom_browser::window_util::IsSingleWebBrowser(browser)) {
    if (browser) {
      BrowserWindow::FromBrowser(browser)->Close();
    }
    return nullptr;
  }

  const bool focus =
      params.focus &&
      params.show_state != ui::mojom::WindowShowState::kMinimized;
  const NavigateParams::WindowAction window_action =
      focus ? NavigateParams::WindowAction::kShowWindow
            : NavigateParams::WindowAction::kShowWindowInactive;
  if (!params.storage) {
    NavigateParams navigate(browser, url, ui::PAGE_TRANSITION_AUTO_TOPLEVEL);
    navigate.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
    navigate.window_action = window_action;
    Navigate(&navigate);
    return browser;
  }

  // The tab is made in its partition first and then put in the window; the
  // navigator does not load a tab it was handed.
  content::WebContents::CreateParams create_contents(
      profile, content::SiteInstance::CreateForFixedStoragePartition(
                   profile, url, *params.storage));
  NavigateParams navigate(browser,
                          content::WebContents::Create(create_contents));
  navigate.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
  navigate.window_action = window_action;
  Navigate(&navigate);
  content::WebContents* inserted = navigate.navigated_or_inserted_contents;
  if (!inserted) {
    // Security: never fall back to a tab of the profile's own session.
    BrowserWindow::FromBrowser(browser)->Close();
    return nullptr;
  }
  inserted->GetController().LoadURL(url, content::Referrer(),
                                    ui::PAGE_TRANSITION_AUTO_TOPLEVEL,
                                    std::string());
  return browser;
}

}  // namespace nexus
