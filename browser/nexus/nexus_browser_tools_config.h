#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_BROWSER_TOOLS_CONFIG_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_BROWSER_TOOLS_CONFIG_H_

#include "base/values.h"

class Profile;

namespace nexus {

// Returns true when BrowserTool should create and host the agent's tabs in a
// dedicated separate browser window instead of the active window that hosts
// the Nexus UI. Backed by the `nexus.browser_tools_separate_window` profile
// pref; defaults to false (shared/active-window behavior) when the profile or
// its PrefService is unavailable.
bool ShouldHostAgentTabsInSeparateWindow(Profile* profile);

// (BrowserEvaluate is now always available — no gate. The former
// `ShouldAllowAgentEvaluate` opt-in was removed; the tool is first-class
// like the rest of the Browser* surface.)

// Returns the effective agent runtime config dict: built-in defaults overlaid
// with the user's stored `nexus.agent_config` overrides. This is the single
// source of truth for agent-config defaults, and the value the browser pushes
// to the nexus_web SharedWorker (boot via the manifest, live via the
// `session.applyAgentConfig` event). Returns just the defaults when the
// profile or its PrefService is unavailable.
base::DictValue GetAgentConfig(Profile* profile);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_BROWSER_TOOLS_CONFIG_H_
