#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_PREF_NAMES_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_PREF_NAMES_H_

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

// Controls where BrowserTool creates and hosts the agent's tabs.
//   false (default) — create tabs in the active window that hosts the Nexus
//                     UI (the tabs live alongside the user's own tabs).
//   true            — create/host the agent's tabs in a dedicated separate
//                     browser window, keeping agent browsing isolated from
//                     the user's current window.
inline constexpr char kNexusBrowserToolsSeparateWindow[] =
    "nexus.browser_tools_separate_window";

// `kNexusBrowserToolsAllowEvaluate` was removed alongside the Settings ·
// Tools opt-in toggle. BrowserEvaluate is now always available — no gate,
// no pref backing it. No pref name is registered anymore.

// Whether right-clicking inside the Nexus guest <webview> offers the
// "Inspect" context-menu item (opens DevTools on the guest). Default
// false: the guest shows NO context menu at all — the panel is a product
// surface, not a page. Toggled by the guest's hidden `/debug inspect
// on|off` slash subcommand via the `ui.setInspectMenuEnabled` bridge
// action; read at right-click time by MaybeHandleNexusGuestContextMenu,
// so flips apply immediately (no reload).
inline constexpr char kNexusInspectMenuEnabled[] =
    "nexus.inspect_menu_enabled";

// Unified agent runtime configuration dictionary. A single dict pref that the
// browser pushes to the nexus_web SharedWorker (boot + live) so the agent
// runtime applies the keys it knows. Keys (added incrementally):
//   maxTurns       (int)    — hard ceiling on the agent loop (next message)
//   stepTimeoutSec (int)    — per-step wall-clock budget in seconds (next turn)
//   model          (dict)   — active selection {providerId, modelId, thinkingTier}
// The transport is generic: any change to this path pushes the whole dict via
// the `session.applyAgentConfig` bridge event; consumers ignore unknown keys.
inline constexpr char kNexusAgentConfig[] = "nexus.agent_config";

// Serialized per-provider configuration dictionary. Each key is a provider ID
// mapping to a dict with api_key, base_url, model, default_model,
// enabled_models, last_verification, etc. This is the SINGLE source of truth
// for provider credentials and settings; the Settings UI reads and writes
// this dict exclusively through the NexusSettings handler
// (`updateProviderConfig`, `resetProviderConfig`). Do not shadow it with
// per-provider flat prefs — one home for each piece of data.
//
// OAuth-backed providers (e.g. "chatgpt-pro") additionally store, in the same
// per-provider dict, the fields written by OAuthTokenStore:
//   oauth_access_token, oauth_refresh_token, oauth_expires_at (unix ms),
//   oauth_account_id, oauth_email
// plus a mirrored `api_key` (= access token) so the existing provider
// Configure()/IsConfigured() path works unchanged. Tokens are plaintext,
// matching how API keys are stored.
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

// True once `LlmBroker` has blanked the `base_url` values that were only a
// stamp of a legacy compiled-in default (see ClearBakedDefaultBaseUrls). The
// cleanup runs once per profile: a value a user types afterwards — even one
// equal to an old default — is a choice, and must survive later boots.
inline constexpr char kNexusBakedDefaultBaseUrlsCleared[] =
    "nexus.baked_default_base_urls_cleared";

// Last `SessionStatus` payload pushed by nexus_web via
// `session.publishStatus`. Cached so the WebUI quick-settings popup can
// render the most-recent selection on cold boot before the guest comes
// online. Schema is intentionally open-ended (provider+model+thinkingTier
// today; agentRunning/threadTitle/etc. tomorrow) — both sides ignore
// unknown keys.
inline constexpr char kNexusLastSessionStatus[] = "nexus.last_session_status";

// Single authoritative store of natively-configured MCP servers. Each key
// is an opaque server id; each value is a dict:
//   { "transport": "stdio", "command": "node", "args": [...],
//     "env": { "API_TOKEN": "…" }, "cwd": "/optional" }
// SECURITY: the MCP analogue of kNexusProviderConfigs — `env` may hold
// secrets (tokens). Read browser-side by NexusMcpServerStore and launched
// natively; the `mcp` bridge capability exposes only each server's id +
// transport to the JS guest, NEVER the command / args / env. The native
// Settings UI (Plan A) is the only writer.
inline constexpr char kNexusMcpServerConfigs[] = "nexus.mcp_server_configs";

}  // namespace nexus::prefs

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_PREF_NAMES_H_
