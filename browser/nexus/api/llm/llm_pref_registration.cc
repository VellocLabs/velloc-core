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
  // full rationale. Stored as a dict so the WebUI can read it directly via
  // GetDict without a JSON parse hop.
  registry->RegisterDictionaryPref(prefs::kNexusTsModelCatalogCache);

  // Most-recent SessionStatus published by nexus_web. Lets the WebUI quick-
  // settings popup render the active selection on cold boot.
  registry->RegisterDictionaryPref(prefs::kNexusLastSessionStatus);

  // Natively-configured MCP servers (id -> {transport, command, args, env,
  // cwd}). Holds secrets in `env`; read browser-side by NexusMcpServerStore,
  // never exposed to the JS guest. See mcp_pref_names.h.
  registry->RegisterDictionaryPref(prefs::kNexusMcpServerConfigs);
}

}  // namespace nexus
