// Shared helpers for locating and opening Custom Browser windows by UI mode.
//
// Centralizes the "find / open / focus a nexus vs. normal browser window"
// logic that is needed from several modules (the native NexusModeButton, the
// WebUI UIViewHandler, and CustomBrowserView initialization) so the behavior
// stays consistent and the at-most-one-kNexus-window invariant lives in one
// place.
#ifndef CUSTOM_BROWSER_UI_VIEW_CUSTOM_BROWSER_WINDOW_UTIL_H_
#define CUSTOM_BROWSER_UI_VIEW_CUSTOM_BROWSER_WINDOW_UTIL_H_

class Browser;
class GURL;
class Profile;

namespace custom_browser::window_util {

// Returns |profile|'s single kNexus window, or nullptr if none exists. Use to
// enforce the at-most-one-kNexus-window invariant.
Browser* FindNexusWindow(Profile* profile);

// Returns a normal (non-kNexus) browser window for |profile|, or nullptr.
Browser* FindNormalWindow(Profile* profile);

// Returns a normal (non-kNexus) browser window for |profile|, creating one if
// none exists. The newly created window comes up in normal mode because no
// pending-kNexus marker is set (see SetPendingNexusWindow). Returns nullptr
// only for a null profile or if window creation fails. Used by BrowserTool to
// host the agent's tabs in a separate window when the user opts in, without
// touching the kNexus chat window.
Browser* FindOrCreateNormalWindow(Profile* profile);

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

}  // namespace custom_browser::window_util

#endif  // CUSTOM_BROWSER_UI_VIEW_CUSTOM_BROWSER_WINDOW_UTIL_H_
