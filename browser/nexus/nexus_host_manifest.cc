#include "custom_browser/browser/nexus/nexus_host_manifest.h"

#include <array>
#include <utility>

#include "base/values.h"
#include "custom_browser/browser/nexus/nexus_bridge_support.h"

namespace nexus {

namespace {

constexpr char kHostKind[] = "nexus";

constexpr std::array<const char*, 15> kBridgeCapabilities = {
    "fs",
    "model",
    "ui",
    "shell",
    // The host actions behind the browser tools (navigate, click,
    // screenshot, …); the runtime defines the tools themselves. Always
    // advertised: the adapter is unconditionally constructed by
    // NexusBridgePortService. Replaced the legacy `glic` capability that
    // used to be advertised here. Session lifecycle (`onStop`,
    // `setAgentLoopSettings`) lives on the dedicated `session` capability.
    "browser",
    // The host actions behind the file and shell tools. The runtime
    // defines the tools themselves and offers them only when these are
    // advertised.
    "files",
    "exec",
    // Phase 12f: dedicated session capability. `onStop` (LLM-session
    // OnStop cascade — renamed from `browser.endSession`),
    // `setAgentLoopSettings` (per-session wall-clock budgets),
    // `setActiveSession`. See
    // custom_browser/browser/nexus/api/session/.
    "session",
    // Span-index transcript search (Ctrl+K dialog). The guest gates the
    // dialog + topbar button on this advertisement, so an older binary
    // simply hides the feature. See custom_browser/browser/nexus/search/.
    "search",
    // Native `sqlite` capability: read/write host SQLite files from the guest
    // (one-shot path form + connection handles). Advertised so consumers gate
    // on it — an older binary that lacks it simply exposes no SQLite surface,
    // which is why adding it needs no interface-version bump. See
    // custom_browser/browser/nexus/api/sqlite/.
    "sqlite",
    // The `remoteAccess` capability: status + configure for the desktop's
    // embedded relay (F-24). The runtime gates its
    // `remoteAccess.getStatus` call on this advertisement — an older binary
    // without it simply exposes no remote-access surface and the runtime
    // makes ZERO calls, which is exactly why adding it needs no
    // interface-version bump. See custom_browser/browser/nexus/remote_access/.
    "remoteAccess",
    // Namespaced, origin-bound secrets (set / has / delete / listNames; no
    // get). Additive: the guest gates every call — and every
    // `{{secret:NAME}}` it puts in a network request — on this
    // advertisement, so an older binary simply has no secret store. See
    // custom_browser/browser/nexus/api/secrets/.
    "secrets",
    // Persisted wall-clock alarms (create / clear / list + the
    // alarms.onAlarm event). Additive: the runtime's scheduler uses it only
    // when advertised, so an older binary keeps the in-page timers. See
    // custom_browser/browser/nexus/api/alarms/.
    "alarms",
    // Single-web windows (open / close / list / focus / setBounds /
    // setState / navigate / back / forward / reload + the onNavigated /
    // onTitleChanged / onClosed events), F-29. Additive: the runtime calls
    // it only when advertised. See
    // custom_browser/browser/nexus/api/web_window/.
    "webWindow",
    // Script channels to a single-web window or a tab in this Nexus window
    // (attach / detach / executeScript / postMessage / addInitScript /
    // removeInitScript + the onMessage / onDetached events), F-29. Additive:
    // the runtime calls it only when advertised. See
    // custom_browser/browser/nexus/api/page_script/.
    "pageScript",
};

// Additive native primitives this host serves, by `<capability>.<action>`
// (or a named variant: `fs.writeFile.ifMatch`). Each one shipped on interface
// 1 without a bump (P-3):
// the guest's wrappers check this list before calling, so on an older host
// — which lacks both the list and the actions — they report "not supported"
// instead of sending a request that host would mishandle. A variant whose
// absence an older host would IGNORE rather than refuse (a new optional
// input) MUST be listed here: the guest cannot learn it any other way.
constexpr auto kNativePrimitives = std::to_array<const char*>({
    "fs.watch",
    "fs.copyFile",
    "fs.symlink",
    "fs.readlink",
    "fs.chmod",
    "fs.writeFile.ifMatch",
    "fs.paths",
    "shell.spawn.argv",
    "shell.spawn.persist",
    "shell.persistentList",
    "shell.persistentKill",
    "shell.env",
    // {{secret:NAME}} in shell.spawn env values, for secrets stored with
    // secrets.set processEnv (an older host would pass the placeholder text
    // through, and refuse a process-only secret's missing origin).
    "shell.spawn.secretEnv",
    "network.streamOpen",
    // httpRequest / streamOpen substitute {{secret:NAME}} for the bound
    // origin (an older host would send the placeholder text verbatim).
    "network.secretTemplates",
    "network.wsOpen",
    // {{oauth:access_token}} / {{oauth:account_id}}: rawStreamOpen, and
    // httpRequest / streamOpen with a providerId (an older host would send
    // the placeholder text).
    "model.rawStreamOpen.oauth",
    "network.oauthTemplates",
    // An empty {{oauth:account_id}} omits its header / query parameter
    // instead of refusing the request (an older host refuses it).
    "model.rawStreamOpen.oauthOptional",
    // httpRequest formParams: an x-www-form-urlencoded body whose values may
    // carry {{secret:NAME}} (an older host would ignore the input and send no
    // body).
    "network.secretFormBody",
    // webWindow.open captureRedirect: a loopback redirect is cancelled and
    // reported as onRedirectCaptured (an older host would ignore the input
    // and let the request reach the port).
    "webWindow.captureRedirect",
    // webWindow.open title: a fixed window title the page cannot change (an
    // older host would ignore the input and show the page's title).
    "webWindow.title",
    // Memory-pressure purge of the guest / GPU / agent-tab processes' caches.
    "appInfo.purgeRenderCaches",
});

template <size_t N>
base::ListValue BuildStringList(const std::array<const char*, N>& entries) {
  base::ListValue list;
  for (const char* entry : entries) {
    list.Append(entry);
  }
  return list;
}

}  // namespace

base::DictValue BuildNexusHostManifest(base::DictValue agent_config,
                                       const NexusHostFeatures& features) {
  base::DictValue manifest;
  manifest.Set("hostKind", kHostKind);

  base::ListValue capabilities = BuildStringList(kBridgeCapabilities);
  // Phase 12g: dropped the `process` capability advertisement.
  // The bridge surface had no JS-side callers (Phase 11b deleted the
  // only consumers — TS BashTool / GrepTool / GlobTool). The C++
  // ProcessTool calls into NexusShellService directly via its own
  // launcher; nothing in the bridge serializes those calls.
  manifest.Set("capabilities", std::move(capabilities));

  // The additive primitives (see kNativePrimitives), plus the ones this
  // particular host has: `shell.spawn.pty` only where a pseudoconsole exists
  // (an older host, or one without a console host, would run a pipe child).
  base::ListValue primitives = BuildStringList(kNativePrimitives);
  if (features.pty_supported) {
    primitives.Append("shell.spawn.pty");
  }
  manifest.Set("primitives", std::move(primitives));

  // A `document` part may sit at the top level of a message, not only inside
  // a tool result. The guest inlines an @-mentioned PDF only when this is
  // set. The host no longer serializes requests (the runtime's provider
  // drivers do, since model.call / streamStart were removed under interface
  // 1), so this is now always true; it is kept because the
  // guest still reads it.
  manifest.Set("llmDocumentParts", true);

  // Boot snapshot of the unified agent runtime config (maxTurns,
  // stepTimeoutSec, model, …). The guest applies this once after building
  // the runtime; later changes arrive live via `session.applyAgentConfig`.
  manifest.Set("agentConfig", std::move(agent_config));

  // No tool definitions here: the runtime defines the host tools itself and
  // offers them by `capabilities` (and `exec.listShells` for the shells).
  return manifest;
}

std::string BuildNexusBridgeBootstrapMessage(
    base::DictValue agent_config,
    const NexusHostFeatures& features) {
  base::DictValue message;
  message.Set("type", "nexus-bridge-bootstrap");
  message.Set("nexus", true);
  message.Set("manifest",
              BuildNexusHostManifest(std::move(agent_config), features));
  return bridge_support::SerializeJson(base::Value(std::move(message)));
}

}  // namespace nexus
