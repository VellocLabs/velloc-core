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

}  // namespace nexus::prefs

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_LLM_LLM_PREF_NAMES_H_
