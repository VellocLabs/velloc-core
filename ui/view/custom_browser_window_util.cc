#include "custom_browser/ui/view/custom_browser_window_util.h"

#include <memory>

#include "base/command_line.h"
#include "base/supports_user_data.h"
#include "build/build_config.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "custom_browser/ui/prefs/custom_browser_ui_prefs.h"
#include "custom_browser/ui/view/custom_browser_view.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
#include "url/gurl.h"

namespace custom_browser::window_util {

namespace {

constexpr char kPendingNexusWindowKey[] = "custom_browser_pending_nexus_window";

// Transient marker attached to a Profile to request that the next
// CustomBrowserView created for it start in kNexus. Never persisted.
class PendingNexusWindowMarker : public base::SupportsUserData::Data {};

constexpr char kPendingExternalUrlWindowKey[] =
    "custom_browser_pending_external_url_window";

// Transient marker attached to a Profile to pin the next CustomBrowserView
// created for it to normal mode — set when the startup code is about to
// create a window for externally-launched URLs. Never persisted.
class PendingExternalUrlWindowMarker : public base::SupportsUserData::Data {};

// Returns the first normal browser window of |profile| whose CustomBrowserView
// is in kNexus mode (when |want_nexus|) or in a browser mode (otherwise).
Browser* FindWindowByMode(Profile* profile, bool want_nexus) {
  Browser* result = nullptr;
  ForEachCurrentBrowserWindowInterfaceOrderedByActivation(
      [&](BrowserWindowInterface* window) {
        Browser* browser = window->GetBrowserForMigrationOnly();
        if (!browser || browser->profile() != profile ||
            !browser->is_type_normal()) {
          return true;
        }
        auto* view = static_cast<CustomBrowserView*>(
            BrowserView::GetBrowserViewForBrowser(browser));
        if (view && view->ui_prefs()) {
          // Pending-aware: a window whose kNexus decision was made in
          // InitializeCustomBrowser() but whose mode hasn't landed yet must
          // already classify as the kNexus window, or external URLs /
          // OpenOrFocusNexusWindow would mis-target it during boot.
          const bool is_nexus = view->IsNexusUIMode();
          if (is_nexus == want_nexus) {
            result = browser;
            return false;  // Stop iterating.
          }
        }
        return true;
      });
  return result;
}

}  // namespace

Browser* FindNexusWindow(Profile* profile) {
  return FindWindowByMode(profile, /*want_nexus=*/true);
}

Browser* FindNormalWindow(Profile* profile) {
  return FindWindowByMode(profile, /*want_nexus=*/false);
}

Browser* FindOrCreateNormalWindow(Profile* profile) {
  if (!profile) {
    return nullptr;
  }
  if (Browser* existing = FindNormalWindow(profile)) {
    return existing;
  }
  // No normal window yet. Create one. We deliberately do NOT call
  // SetPendingNexusWindow, so the new CustomBrowserView consumes a false
  // pending-nexus marker during init and comes up in normal (non-kNexus)
  // mode. OpenEmptyWindow returns the Browser* and seeds it with a valid
  // initial tab, so the window is never in a tabless state.
  // custom-browser: Chromium 149's OpenEmptyWindow returns
  // BrowserWindowInterface*; convert to Browser* for callers.
  auto* bwi = chrome::OpenEmptyWindow(
      profile, /*should_trigger_session_restore=*/false);
  return bwi ? bwi->GetBrowserForMigrationOnly() : nullptr;
}

void OpenOrFocusNexusWindow(Profile* profile) {
  if (!profile) {
    return;
  }
  if (Browser* nexus = FindNexusWindow(profile)) {
    nexus->window()->Activate();
    return;
  }
  SetPendingNexusWindow(profile);
  chrome::NewEmptyWindow(profile, /*should_trigger_session_restore=*/false);
}

void OpenOrFocusNormalWindow(Profile* profile) {
  if (!profile) {
    return;
  }
  if (Browser* browser = FindNormalWindow(profile)) {
    browser->window()->Activate();
    return;
  }
  chrome::NewEmptyWindow(profile, /*should_trigger_session_restore=*/false);
}

void OpenUrlInNormalWindow(Profile* profile, const GURL& url) {
  if (!profile || !url.is_valid()) {
    return;
  }

  // Prefer an existing normal (non-kNexus) window: add a foreground tab and
  // raise it. This keeps the click out of the kNexus window so the nexus
  // chat pane is never replaced.
  if (Browser* target = FindNormalWindow(profile)) {
    NavigateParams params(target, url, ui::PAGE_TRANSITION_LINK);
    params.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
    Navigate(&params);
    target->window()->Activate();
    return;
  }

  // No normal window yet: open the URL in a brand-new normal window rather
  // than reusing the kNexus window.
  NavigateParams params(profile, url, ui::PAGE_TRANSITION_LINK);
  params.disposition = WindowOpenDisposition::NEW_WINDOW;
  Navigate(&params);
  if (params.browser) {
    // NavigateParams::browser is a BrowserWindowInterface* (no window()); use
    // the interface's GetWindow() → ui::BaseWindow::Activate().
    params.browser->GetWindow()->Activate();
  }
}

void SetPendingNexusWindow(Profile* profile) {
  if (!profile) {
    return;
  }
  profile->SetUserData(kPendingNexusWindowKey,
                       std::make_unique<PendingNexusWindowMarker>());
}

bool ConsumePendingNexusWindow(Profile* profile) {
  if (!profile || !profile->GetUserData(kPendingNexusWindowKey)) {
    return false;
  }
  profile->RemoveUserData(kPendingNexusWindowKey);
  return true;
}

bool IsNexusBrowser(Browser* browser) {
  if (!browser || !browser->is_type_normal()) {
    return false;
  }
  auto* view = static_cast<CustomBrowserView*>(
      BrowserView::GetBrowserViewForBrowser(browser));
  return view && view->IsNexusUIMode();
}

Browser* AdjustBrowserForExternalUrlOpen(Profile* profile,
                                         Browser* candidate) {
  if (candidate && !IsNexusBrowser(candidate)) {
    // Common case: the startup code already picked a normal window.
    return candidate;
  }
  if (candidate) {
    // The picked window is the kNexus chat window — reroute to any existing
    // normal window instead (FindNormalWindow ignores workspaces; landing on
    // another workspace beats landing in the chat window).
    if (Browser* normal = FindNormalWindow(profile)) {
      return normal;
    }
  }
  // No usable window: returning null makes the startup code create a fresh
  // Browser for the URLs. Pin that upcoming window to normal mode — for a
  // profile whose startup default hasn't been consumed yet (e.g. the first
  // window of --profile-directory=X opened via a link), it would otherwise
  // come up in kNexus with the URL inside it.
  SetPendingExternalUrlWindow(profile);
  return nullptr;
}

void SetPendingExternalUrlWindow(Profile* profile) {
  if (!profile) {
    return;
  }
  profile->SetUserData(kPendingExternalUrlWindowKey,
                       std::make_unique<PendingExternalUrlWindowMarker>());
}

bool ConsumePendingExternalUrlWindow(Profile* profile) {
  if (!profile || !profile->GetUserData(kPendingExternalUrlWindowKey)) {
    return false;
  }
  profile->RemoveUserData(kPendingExternalUrlWindowKey);
  return true;
}

bool HasExternalUrlStartupArgs() {
  // `about:blank` is exempt: the e2e harness appends it to every launch
  // purely to keep the NTP tab from spawning (see the "Why the trailing
  // about:blank" note in nexus_web/e2e/harness/launchBrowser.ts). Nothing
  // externally opens about:blank, so it must not demote the startup window
  // from kNexus.
  constexpr base::CommandLine::StringViewType kAboutBlankPlaceholder =
#if BUILDFLAG(IS_WIN)
      L"about:blank";
#else
      "about:blank";
#endif
  for (const auto& arg : base::CommandLine::ForCurrentProcess()->GetArgs()) {
    if (arg != kAboutBlankPlaceholder) {
      return true;
    }
  }
  return false;
}

}  // namespace custom_browser::window_util
