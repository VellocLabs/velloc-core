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

// Shows the session-crashed bubble on |browser| once its popup anchor is
// visible: at once when the window is already shown, otherwise on the first
// show of its fallback popup anchor. Startup tabs are inserted before the
// window is shown, when the bubble has nothing to anchor to.
void ShowSessionCrashedBubbleWhenAnchored(BrowserWindowInterface* browser,
                                          bool skip_tab_checking);

// Consumes the deferred-crash-bubble marker for |browser|'s profile (if
// any) and, from a posted task, shows the session-crashed bubble on |browser|
// through ShowSessionCrashedBubbleWhenAnchored — the consume site runs during
// Widget::Init, before startup tabs exist. No-op without a marker.
void MaybeShowDeferredCrashBubble(BrowserWindowInterface* browser);

}  // namespace custom_browser::nexus_startup_ui

#endif  // CUSTOM_BROWSER_UI_STARTUP_NEXUS_STARTUP_PROMPTS_H_
