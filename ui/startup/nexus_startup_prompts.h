// Suppresses Chromium's startup prompt chain on the kNexus window, deferring
// the prompts to normal browser windows. The crash bubble is stashed as a
// profile marker and shown by the next normal-type window.
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
void MaybeShowDeferredCrashBubble(BrowserWindowInterface* browser);

}  // namespace custom_browser::nexus_startup_ui

#endif  // CUSTOM_BROWSER_UI_STARTUP_NEXUS_STARTUP_PROMPTS_H_
