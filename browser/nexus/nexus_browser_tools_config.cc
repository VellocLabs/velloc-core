#include "custom_browser/browser/nexus/nexus_browser_tools_config.h"

#include "chrome/browser/profiles/profile.h"
#include "components/prefs/pref_service.h"
#include "custom_browser/browser/nexus/api/llm/llm_pref_names.h"
// `prefs::kNexusBrowserToolsAllowEvaluate` is gone — the include of
// `llm_pref_names.h` is still needed by `ShouldHostAgentTabsInSeparateWindow`
// + `GetAgentConfig` below.

namespace nexus {

bool ShouldHostAgentTabsInSeparateWindow(Profile* profile) {
  if (!profile) {
    return false;
  }
  PrefService* prefs = profile->GetPrefs();
  if (!prefs) {
    return false;
  }
  return prefs->GetBoolean(prefs::kNexusBrowserToolsSeparateWindow);
}

base::DictValue GetAgentConfig(Profile* profile) {
  // Built-in defaults — the single source of truth. Keys mirror the wire
  // contract consumed by nexus_web's QueryEngine.applyRuntimeSettings:
  //   maxTurns       — hard ceiling on the agent loop (default 100).
  //   stepTimeoutSec — per-step wall-clock budget in seconds (default 180).
  base::DictValue config;
  config.Set("maxTurns", 100);
  config.Set("stepTimeoutSec", 180);

  if (!profile) {
    return config;
  }
  PrefService* prefs = profile->GetPrefs();
  if (!prefs) {
    return config;
  }

  // Overlay the user's stored overrides (and any future keys, e.g. `model`).
  // Dict::Merge deep-merges nested dicts, so a partial `model` override merges
  // cleanly with future default sub-keys.
  config.Merge(prefs->GetDict(prefs::kNexusAgentConfig).Clone());
  return config;
}

}  // namespace nexus
