#include "custom_browser/ui/view/custom_browser_window_util.h"

#include <memory>
#include <string_view>

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
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/common/chrome_switches.h"
#include "custom_browser/browser/nexus/panel/nexus_background_panel_host.h"
#include "custom_browser/common/custom_browser_switches.h"
#include "custom_browser/ui/prefs/custom_browser_ui_prefs.h"
#include "custom_browser/ui/view/custom_browser_view.h"
#include "custom_browser/ui/view/relaunch_command_line.h"
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
  return HasExternalUrlArgs(*base::CommandLine::ForCurrentProcess());
}

bool HandlePlainRelaunch(Profile* profile,
                         const base::CommandLine& command_line) {
  if (!profile) {
    return false;
  }
  // Switches that ask for a specific kind of window. Any of them means the
  // launch is not "just open the product" and Chromium's own handling wins:
  // --app/--app-id (PWA / shortcut app window), --incognito, the launch-on-
  // startup --no-startup-window (nothing should open at all), and our own
  // --browser-mode escape hatch (explicitly a normal browser window).
  // ...plus launches that carry their own action and are dispatched deeper in
  // ProcessCmdLineImpl: a toast click (--notification-launch-id), a jump-list
  // task, session restore, the guest-window and app-uninstall launches.
  const std::string_view kWindowKindSwitches[] = {
      ::switches::kApp,
      ::switches::kAppId,
      ::switches::kIncognito,
      ::switches::kNoStartupWindow,
      ::switches::kGuest,
      ::switches::kRestoreLastSession,
      ::switches::kNotificationLaunchId,
      ::switches::kWinJumplistAction,
      ::switches::kUninstallAppId,
      custom_browser::switches::kBrowserMode,
  };
  if (!IsPlainRelaunchCommandLine(command_line, kWindowKindSwitches)) {
    return false;
  }
  // The relaunch names the last-used profile, but the page the user wants
  // back may be parked for another loaded profile (background panel): that
  // profile's window is the one to reopen, or its sessions run invisibly
  // with nothing ever taking them back.
  if (auto* host = nexus::NexusBackgroundPanelHostFactory::GetForProfile(profile);
      !host || !host->has_parked()) {
    if (ProfileManager* profile_manager = g_browser_process->profile_manager()) {
      for (Profile* loaded : profile_manager->GetLoadedProfiles()) {
        auto* other = nexus::NexusBackgroundPanelHostFactory::GetForProfile(loaded);
        if (other && other->has_parked()) {
          profile = loaded;
          break;
        }
      }
    }
  }
  OpenOrFocusNexusWindow(profile);
  return true;
}

}  // namespace custom_browser::window_util
