#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_CLIENT_RESTART_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_CLIENT_RESTART_H_

namespace nexus {

// Restarts the client, restoring the windows and tabs that were open.
//
// The single home for "how Velloc restarts", because two surfaces need it and
// the reasoning behind the call is not obvious from the call site:
//
//   * `appInfo.relaunch` on the guest bridge — the About page's "Restart now"
//     button, which applies a new runtime source / update channel;
//   * `restartBrowser` on the panel WebUI — the guest-recovery overlay, which
//     paints in the host page precisely when the guest bridge is unusable and
//     therefore cannot go through the route above.
//
// `chrome::AttemptRestart`, not `chrome::AttemptRelaunch`. Both end the process
// and start a new one; only Restart sets `kWasRestarted`, which is what makes
// the new process restore the session. That is what chrome://settings gets from
// its own Relaunch button (RestartType::RESTART →
// BrowserLifetimeHandler::HandleRestart), and losing the user's tabs to apply a
// runtime-source change would be a bad trade.
//
// The restart is POSTED, not run inline: it tears down browser-wide state, and
// both callers are invoked from inside a message/bridge dispatcher that still
// owns the request further up the stack. Posting also means a caller that wants
// to answer its client first can simply do so before calling this — same task
// runner, FIFO order.
//
// Must be called on the UI thread.
void RestartClient();

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_CLIENT_RESTART_H_
