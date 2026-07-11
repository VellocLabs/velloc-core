#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_LLM_LLM_PREF_REGISTRATION_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_LLM_LLM_PREF_REGISTRATION_H_

class PrefRegistrySimple;

namespace nexus {

// Registers all `nexus.*` profile preferences with PrefService. Called once
// during browser startup from the profile pref registration path.
void RegisterNexusProfilePrefs(PrefRegistrySimple* registry);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_LLM_LLM_PREF_REGISTRATION_H_
