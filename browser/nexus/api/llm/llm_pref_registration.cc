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

  // Where BrowserTool hosts the agent's tabs: false = active (Nexus) window,
  // true = a dedicated separate browser window. Default preserves the legacy
  // shared-window behavior.
  registry->RegisterBooleanPref(prefs::kNexusBrowserToolsSeparateWindow, false);

  // `kNexusBrowserToolsAllowEvaluate` (the BrowserEvaluate opt-in) was
  // removed — BrowserEvaluate is now always available, no pref backs it.
  // Existing Profile prefs files keep the stale key as orphan data;
  // chrome's pref store tolerates unknown keys.

  // "Inspect" context-menu item inside the Nexus guest <webview>. Default
  // OFF — the guest shows no context menu at all. Flipped via the hidden
  // `/debug inspect on|off` slash subcommand (ui.setInspectMenuEnabled).
  registry->RegisterBooleanPref(prefs::kNexusInspectMenuEnabled, false);

  // Unified agent runtime config dict, pushed to the nexus_web SharedWorker.
  // Starts empty; keys (maxTurns, stepTimeoutSec, model) are layered on top of
  // GetAgentConfig() defaults as the user changes settings.
  registry->RegisterDictionaryPref(prefs::kNexusAgentConfig);

  // Single authoritative store of per-provider configuration. Every provider
  // id maps to a dict holding api_key, base_url, model, default_model,
  // enabled_models, last_verification, etc.
  registry->RegisterDictionaryPref(prefs::kNexusProviderConfigs);

  // Cached raw /v1/models JSON per provider. Written by LlmBroker on every
  // successful fetch and read at profile startup so the TS layer has a
  // catalogue immediately instead of waiting on the network.
  registry->RegisterDictionaryPref(prefs::kNexusProviderCatalogs);

  // Curated TS-side catalogue mirror — see kNexusTsModelCatalogCache for the
  // full rationale. Stored as a dict so it can be read via GetDict without a
  // JSON parse hop.
  registry->RegisterDictionaryPref(prefs::kNexusTsModelCatalogCache);

  // One-shot marker for the legacy default-base-URL cleanup — see
  // kNexusBakedDefaultBaseUrlsCleared.
  registry->RegisterBooleanPref(prefs::kNexusBakedDefaultBaseUrlsCleared,
                                false);

  // Most-recent SessionStatus published by nexus_web — write-only today; see
  // kNexusLastSessionStatus.
  registry->RegisterDictionaryPref(prefs::kNexusLastSessionStatus);

  // Natively-configured MCP servers (id -> {transport, command, args, env,
  // cwd}). Holds secrets in `env`; read browser-side by NexusMcpServerStore,
  // never exposed to the JS guest. See mcp_pref_names.h.
  registry->RegisterDictionaryPref(prefs::kNexusMcpServerConfigs);

  // Remote access (plan §8.3): default OFF, no bind address, the well-known
  // default port. See the pref-name comments in llm_pref_names.h.
  registry->RegisterBooleanPref(prefs::kNexusRemoteAccessEnabled, false);
  registry->RegisterStringPref(prefs::kNexusRemoteAccessBindAddress,
                               std::string());
  registry->RegisterIntegerPref(prefs::kNexusRemoteAccessPort, 47615);
  // The persisted access-link pairings (see llm_pref_names.h). A list pref:
  // each element is one pairing dict. Starts empty.
  registry->RegisterListPref(prefs::kNexusRemoteAccessPairings);
}

}  // namespace nexus
