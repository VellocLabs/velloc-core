#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_CLIENT_RESTART_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_CLIENT_RESTART_H_

namespace nexus {

// Restarts the client, restoring its windows and tabs: `chrome::AttemptRestart`
// sets `kWasRestarted`, which AttemptRelaunch does not. Posted, not run inline,
// since callers sit inside a dispatcher that still owns the request. UI thread.
void RestartClient();

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_CLIENT_RESTART_H_
