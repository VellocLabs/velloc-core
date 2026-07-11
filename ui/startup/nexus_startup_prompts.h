// Suppresses Chromium's startup prompt chain (session-crashed bubble,
// Windows launch-on-startup infobar, session-restore infobar, bad-flags
// prompt, default-browser / pin-to-taskbar / PDF infobars) on the kNexus
// window, deferring the prompts to normal browser windows.
//
// Wiring:
//  - chrome/browser/ui/startup/infobar_utils.cc calls
//    MaybeSuppressStartupPrompts() from an ENABLE_CUSTOM_BROWSER hook placed
//    after the automation/testing banners (those still show) and BEFORE the
//    static `infobars_shown` first-profile flag flips — so a later launch
//    that lands in a normal window re-evaluates the whole chain (natural
//    deferral for everything but the crash bubble).
//  - The crash bubble's HasPendingUncleanExit() gate burns the moment the
//    startup (Nexus) window opens, so suppression stashes a transient
//    profile marker; CustomBrowserView::InitializeCustomBrowser() consumes
//    it via MaybeShowDeferredCrashBubble() on the next normal-type window
//    (whether that window comes from a link open, Ctrl+N, or the tray).
#ifndef CUSTOM_BROWSER_UI_STARTUP_NEXUS_STARTUP_PROMPTS_H_
#define CUSTOM_BROWSER_UI_STARTUP_NEXUS_STARTUP_PROMPTS_H_

class Browser;
class BrowserWindowInterface;

namespace custom_browser::nexus_startup_ui {

// Returns true when |browser| is the kNexus window (pending-aware — at
// startup-prompt time the kNexus mode has not landed yet) and the startup
// prompt chain must be skipped for it. Side effect: when the profile has a
// pending unclean exit, stashes the deferred-crash-bubble marker so the
// restore offer re-surfaces on the next normal window.
bool MaybeSuppressStartupPrompts(BrowserWindowInterface* browser);

// Consumes the deferred-crash-bubble marker for |browser|'s profile (if
// any) and shows the session-crashed bubble on |browser| once the current
// task completes — the consume site runs during Widget::Init, before
// startup tabs exist. No-op without a marker.
void MaybeShowDeferredCrashBubble(Browser* browser);

}  // namespace custom_browser::nexus_startup_ui

#endif  // CUSTOM_BROWSER_UI_STARTUP_NEXUS_STARTUP_PROMPTS_H_
