#include "custom_browser/browser/nexus/api/llm/llm_pref_registration.h"

#include "components/prefs/pref_registry_simple.h"
#include "custom_browser/browser/nexus/api/llm/llm_pref_names.h"

namespace nexus {

void RegisterNexusProfilePrefs(PrefRegistrySimple* registry) {
  // --- Global Nexus prefs ---
  registry->RegisterBooleanPref(prefs::kNexusEnabled, false);
  registry->RegisterBooleanPref(prefs::kNexusCompletedOnboarding, false);
  registry->RegisterStringPref(prefs::kNexusActiveProviderId, std::string());
  registry->RegisterStringPref(prefs::kNexusWebappUrl, std::string());

  // Single authoritative store of per-provider configuration. Every provider
  // id maps to a dict holding api_key, base_url, model, default_model,
  // enabled_models, last_verification, etc.
  registry->RegisterDictionaryPref(prefs::kNexusProviderConfigs);

  // Cached raw /v1/models JSON per provider. Written by LlmBroker on every
  // successful fetch and read at profile startup so the TS layer has a
  // catalogue immediately instead of waiting on the network.
  registry->RegisterDictionaryPref(prefs::kNexusProviderCatalogs);

  // Curated TS-side catalogue mirror — see kNexusTsModelCatalogCache for the
  // full rationale. Stored as a dict so the WebUI can read it directly via
  // GetDict without a JSON parse hop.
  registry->RegisterDictionaryPref(prefs::kNexusTsModelCatalogCache);

  // Most-recent SessionStatus published by nexus_web. Lets the WebUI quick-
  // settings popup render the active selection on cold boot.
  registry->RegisterDictionaryPref(prefs::kNexusLastSessionStatus);
}

}  // namespace nexus
