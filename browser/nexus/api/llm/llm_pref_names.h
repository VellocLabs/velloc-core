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

// True once the guest runtime has finished booting on this profile. Until
// then every launch is a first run: the boot welcome hides the window caption
// buttons and, after a delay, tells the user the first start takes longer.
inline constexpr char kNexusRuntimeFirstBootCompleted[] =
    "nexus.runtime_first_boot_completed";

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

// Per-provider configuration dict keyed by provider ID (api_key, base_url,
// model, default_model, enabled_models, last_verification, …) — the single
// source of truth for provider credentials and settings. OAuth providers add
// the OAuthTokenStore fields (oauth_*) plus a mirrored `api_key`; tokens are
// plaintext, matching how API keys are stored.
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
// updates independently of chrome.exe); this pref keeps the last snapshot
// for the next browser start, so LlmBroker can apply the guest-owned
// default base URLs (and NexusLlmHost's BuildCatalog can graft curated
// models) *before* the guest finishes loading. Stored as a dictionary
// value (not raw JSON) so we can copy/clone without re-parsing.
inline constexpr char kNexusTsModelCatalogCache[] =
    "nexus.ts_model_catalog_cache";

// True once `LlmBroker` has blanked the `base_url` values that were only a
// stamp of a legacy compiled-in default (see ClearBakedDefaultBaseUrls). The
// cleanup runs once per profile: a value a user types afterwards — even one
// equal to an old default — is a choice, and must survive later boots.
inline constexpr char kNexusBakedDefaultBaseUrlsCleared[] =
    "nexus.baked_default_base_urls_cleared";

// --- Remote access (F-24) ---
// The desktop's embedded relay: a default-OFF net::HttpServer that reverse-
// proxies the guest origin's immutable build entry and exposes /rt so a
// remote browser on the LAN/VPN can join this desktop's runtime. Owned
// C++-side (NexusRemoteAccessService); the nexus_web Settings page drives
// them through the `remoteAccess` bridge capability, never directly.
//
// Whether the listener runs at all. Default OFF — remote access is opt-in.
inline constexpr char kNexusRemoteAccessEnabled[] = "nexus.remote_access.enabled";
// The single IPv4 literal the listener binds. Must currently appear in
// net::GetNetworkList(); empty or absent-from-list → not listening. NEVER
// "0.0.0.0" / "::". "127.0.0.1" is accepted for local debugging.
inline constexpr char kNexusRemoteAccessBindAddress[] =
    "nexus.remote_access.bind_address";
// The listener port, 1024–65535. A port already in use surfaces as the
// `port_in_use` status — the listener never silently switches ports (the
// minted link would die).
inline constexpr char kNexusRemoteAccessPort[] = "nexus.remote_access.port";

// Contract: the persisted remote-access pairings, one dict per minted link,
// reloaded by the runtime on boot: pairId (base64url, 16 bytes), secret
// (base64url, 32 bytes), scope { mode, sessions }, label, createdAt and
// expiresAt (unix ms), revoked (bool), lastSeenAt (optional, unix ms).
// Security: `secret` is stored plaintext on disk and never crosses the wire,
// the same exposure class as the stored API keys. The runtime is the only
// reader and writer; C++ stores the list verbatim. Readers ignore unknown keys.
inline constexpr char kNexusRemoteAccessPairings[] =
    "nexus.remote_access.pairings";

// The `secrets` bridge capability's store (api/secrets/). A dict keyed by
// the namespaced secret name (`provider:…`, `plugin:…`, `mcp:…`); each
// value is {value: NexusSecretCrypt-sealed string, origin: the serialized
// url::Origin the secret is bound to}. Written only by NexusSecretStore;
// the plaintext never leaves the browser process — there is no `get`, a
// secret is only substituted into a request to its bound origin.
inline constexpr char kNexusSecrets[] = "nexus.secrets";

// The `alarms` capability's alarm set (api/alarms/): {name: {whenMs,
// periodMs?}}, wall-clock ms since the epoch. NexusAlarmService is the only
// writer; it survives a restart, and alarms that came due while the browser
// was not running fire once when the profile loads.
inline constexpr char kNexusAlarms[] = "nexus.alarms";

}  // namespace nexus::prefs

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_PREF_NAMES_H_
