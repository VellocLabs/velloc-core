#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_LLM_LLM_PREF_NAMES_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_LLM_LLM_PREF_NAMES_H_

namespace nexus::prefs {

// Whether the user has consented to the agent feature.
inline constexpr char kNexusEnabled[] = "nexus.enabled";

// Whether onboarding/consent flow is complete.
inline constexpr char kNexusCompletedOnboarding[] =
    "nexus.completed_onboarding";

// Currently selected LLM provider ID (e.g. "openai", "anthropic").
inline constexpr char kNexusActiveProviderId[] = "nexus.active_provider_id";

// Custom agent webapp URL (empty string = use default).
inline constexpr char kNexusWebappUrl[] = "nexus.webapp_url";

// Serialized per-provider configuration dictionary. Each key is a provider ID
// mapping to a dict with api_key, base_url, model, default_model,
// enabled_models, last_verification, etc. This is the SINGLE source of truth
// for provider credentials and settings; the Settings UI reads and writes
// this dict exclusively through the NexusSettings handler
// (`updateProviderConfig`, `resetProviderConfig`). Do not shadow it with
// per-provider flat prefs — one home for each piece of data.
inline constexpr char kNexusProviderConfigs[] = "nexus.provider_configs";

// Cached raw model-catalogue JSON from each provider's /v1/models (or
// equivalent) endpoint. Keyed by provider id; each value is a string holding
// the last successful response body verbatim. The TS layer reads this to
// render the catalogue without waiting on a live fetch, and to access
// provider-specific fields (e.g. inputTokenLimit) that the parsed
// ModelInfo struct does not carry. Refreshed on every browser start for
// providers that have credentials configured.
inline constexpr char kNexusProviderCatalogs[] = "nexus.provider_catalogs";

// Cached copy of the curated TS-side `NEXUS_MODEL_CONFIG`, pushed by
// nexus_web at boot via the `model.publishCatalog` bridge route. The TS
// app remains the source of truth for the catalogue (it ships online and
// updates independently of chrome.exe); this pref exists so the WebUI
// `<nexus-quick-settings>` popup has a usable catalogue available at the
// next browser start *before* the guest finishes loading. Stored as a
// dictionary value (not raw JSON) so we can copy/clone without re-parsing.
inline constexpr char kNexusTsModelCatalogCache[] =
    "nexus.ts_model_catalog_cache";

// Last `SessionStatus` payload pushed by nexus_web via
// `session.publishStatus`. Cached so the WebUI quick-settings popup can
// render the most-recent selection on cold boot before the guest comes
// online. Schema is intentionally open-ended (provider+model+thinkingTier
// today; agentRunning/threadTitle/etc. tomorrow) — both sides ignore
// unknown keys.
inline constexpr char kNexusLastSessionStatus[] = "nexus.last_session_status";

}  // namespace nexus::prefs

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_LLM_LLM_PREF_NAMES_H_
