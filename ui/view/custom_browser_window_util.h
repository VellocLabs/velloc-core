// Shared helpers for locating and opening Custom Browser windows by UI mode.
//
// Centralizes the "find / open / focus a nexus vs. normal browser window"
// logic that is needed from several modules (the WebUI UIViewHandler,
// BrowserTool, and CustomBrowserView initialization) so the behavior stays
// consistent and the at-most-one-kNexus-window invariant lives in one place.
#ifndef CUSTOM_BROWSER_UI_VIEW_CUSTOM_BROWSER_WINDOW_UTIL_H_
#define CUSTOM_BROWSER_UI_VIEW_CUSTOM_BROWSER_WINDOW_UTIL_H_

#include <optional>
#include <string>

class Browser;
class BrowserWindowInterface;
class GURL;
class Profile;

namespace base {
class CommandLine;
}

namespace custom_browser::window_util {

// Returns |profile|'s single kNexus window, or nullptr if none exists. Use to
// enforce the at-most-one-kNexus-window invariant.
BrowserWindowInterface* FindNexusWindow(Profile* profile);

// Returns a normal (non-kNexus) browser window for |profile|, or nullptr.
BrowserWindowInterface* FindNormalWindow(Profile* profile);

// Returns a normal (non-kNexus) browser window for |profile|, creating one if
// none exists. The newly created window comes up in normal mode because no
// pending-kNexus marker is set (see SetPendingNexusWindow). Returns nullptr
// only for a null profile or if window creation fails. Used by BrowserTool to
// host the agent's tabs in a separate window when the user opts in, without
// touching the kNexus chat window.
BrowserWindowInterface* FindOrCreateNormalWindow(Profile* profile);

// Focuses |profile|'s existing kNexus window if present; otherwise opens a new
// window that starts in kNexus (singleton-enforced at window init).
void OpenOrFocusNexusWindow(Profile* profile);

// Focuses an existing normal (non-kNexus) browser window for |profile| if
// present; otherwise opens a new normal browser window. Never changes the
// caller window's UI mode.
void OpenOrFocusNormalWindow(Profile* profile);

// Opens |url| in a new foreground tab of |profile|'s normal (non-kNexus)
// browser window, activating that window. If no normal window exists, opens
// |url| in a brand-new normal window. No-op for a null profile or an invalid
// URL. Never navigates the kNexus window, so the nexus chat pane is left
// intact — this is the destination used when a hyperlink is clicked inside
// the chat (see NexusUiBridgeAdapter::HandleOpenUrlOnUI).
void OpenUrlInNormalWindow(Profile* profile, const GURL& url);

// Marks the next CustomBrowserView created for |profile| to start in kNexus.
// Consumed (cleared) by ConsumePendingNexusWindow during that view's init.
// Backed by transient Profile user-data, so it is never persisted.
void SetPendingNexusWindow(Profile* profile);
bool ConsumePendingNexusWindow(Profile* profile);

// Marks the next CustomBrowserView created for |profile| as a single-web
// window (kSingleWeb, F-29) with the fixed |title| (empty: the page's own
// title). The caller sets it immediately before its own synchronous
// Browser::Create, which consumes it during view init; it takes precedence
// over every other marker. Transient Profile user-data. Consume returns
// nullopt when no marker was set, otherwise its title.
void SetPendingSingleWebWindow(Profile* profile,
                               std::u16string title = std::u16string());
std::optional<std::u16string> ConsumePendingSingleWebWindow(Profile* profile);

// True when |browser| is a single-web window. Such a window is never returned
// by FindNexusWindow / FindNormalWindow and never receives external URLs.
// Null-safe.
bool IsSingleWebBrowser(BrowserWindowInterface* browser);

// True when |browser| is (or is becoming) the profile's kNexus window: a
// normal-type browser whose CustomBrowserView reports IsNexusUIMode(). The
// pending-aware predicate matters: during boot the kNexus decision is made
// in InitializeCustomBrowser() but the mode only lands once the WebUI loads
// its layout params, and routing/suppression decisions (startup tabs,
// startup prompts) fire inside that gap. Null-safe.
bool IsNexusBrowser(BrowserWindowInterface* browser);

// Routing policy for externally-opened URLs (OS default-browser opens /
// second-instance command lines): the kNexus window is never the target.
// Returns |candidate| unchanged when it is a non-kNexus window; otherwise
// prefers an existing normal window; otherwise marks the profile (see
// SetPendingExternalUrlWindow) and returns nullptr so the startup code
// creates a fresh window that is pinned to normal mode. Called from the
// ENABLE_CUSTOM_BROWSER hook in GetExistingBrowserForOpenBehavior
// (chrome/browser/ui/startup/startup_browser_creator_impl.cc).
BrowserWindowInterface* AdjustBrowserForExternalUrlOpen(Profile* profile, BrowserWindowInterface* candidate);

// Marks the next CustomBrowserView created for |profile| as a window opened
// to host externally-launched URLs: it stays in normal mode even when the
// profile's startup default would otherwise make it kNexus (e.g. the first
// window of a secondary profile launched via a URL). Transient Profile
// user-data, exact mirror of the pending-kNexus marker; takes precedence
// over it for the window that consumes it.
void SetPendingExternalUrlWindow(Profile* profile);
bool ConsumePendingExternalUrlWindow(Profile* profile);

// True when the current process command line carries URL/file arguments,
// i.e. this launch exists to open something (an OS default-browser open).
// The bare `about:blank` placeholder is exempt: the e2e harness appends it
// to every launch to keep the NTP from spawning, and it must not demote the
// first window from kNexus. Thin wrapper over HasExternalUrlArgs()
// (relaunch_command_line.h) for the process command line.
bool HasExternalUrlStartupArgs();

// Second-instance routing for a plain relaunch (IsPlainRelaunchCommandLine):
// focuses the profile's kNexus window, or opens one. Returns true when handled
// (the caller opens nothing else); false lets Chromium's own handling proceed.
// A null |profile| is never handled.
bool HandlePlainRelaunch(Profile* profile,
                         const base::CommandLine& command_line);

}  // namespace custom_browser::window_util

#endif  // CUSTOM_BROWSER_UI_VIEW_CUSTOM_BROWSER_WINDOW_UTIL_H_
