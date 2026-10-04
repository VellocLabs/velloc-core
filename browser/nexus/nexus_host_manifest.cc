#include "custom_browser/browser/nexus/nexus_host_manifest.h"

#include <array>
#include <utility>

#include "base/values.h"
#include "custom_browser/browser/nexus/nexus_bridge_support.h"

namespace nexus {

namespace {

constexpr char kHostKind[] = "nexus";

constexpr std::array<const char*, 11> kBridgeCapabilities = {
    "fs",
    "model",
    "ui",
    "shell",
    // Native-backed MCP (stdio + Streamable HTTP). The guest gates its
    // native MCP pool on `supportsBridgeCapability('mcp')` (see
    // createBrowserRuntime.ts), so the capability must be advertised here
    // for the `NexusMcpBridgeAdapter` (registered unconditionally by
    // NexusBridgePortService) to be reachable. See
    // custom_browser/browser/nexus/api/mcp/.
    "mcp",
    // The macro-DSL-driven browser tool catalog. Always advertised: the
    // adapter is unconditionally constructed by NexusBridgePortService
    // and its catalog is the source of truth for the LLM-facing browser
    // tools. See custom_browser/browser/nexus/tools/.
    //
    // Replaced the legacy `glic` capability that used to be advertised
    // here. Every browser-actor tool now arrives via
    // `browser.<tool_name>`. Session lifecycle (`onStop`,
    // `setAgentLoopSettings`) moved to the dedicated `session`
    // capability in Phase 12f.
    "browser",
    // Phase 12f: dedicated session capability. `publishStatus`
    // (SessionStatus cache in LlmBroker), `onStop` (LLM-session
    // OnStop cascade — renamed from `browser.endSession`),
    // `setAgentLoopSettings` (per-session wall-clock budgets). See
    // custom_browser/browser/nexus/api/session/.
    "session",
    // Span-index transcript search (Ctrl+K dialog). The guest gates the
    // dialog + topbar button on this advertisement, so an older binary
    // simply hides the feature. See custom_browser/browser/nexus/search/.
    "search",
    // Phase 12: catalog metadata served lazily over the bridge. JS
    // pulls `listHostedTools` (replacing the bootstrap manifest's
    // hostedTools field) and `renderPromptSection` (replacing TS-side
    // getUsingYourToolsSection rendering). See
    // custom_browser/browser/nexus/api/catalog/.
    "catalog",
    // Native `sqlite` capability: read/write host SQLite files from the guest
    // (one-shot path form + connection handles). Advertised so consumers gate
    // on it — an older binary that lacks it simply exposes no SQLite surface,
    // which is why adding it needs no interface-version bump. See
    // custom_browser/browser/nexus/api/sqlite/.
    "sqlite",
    // The `remoteAccess` capability: status + configure for the desktop's
    // embedded relay (remote-browser-client plan §8). The runtime gates its
    // `remoteAccess.getStatus` call on this advertisement — an older binary
    // without it simply exposes no remote-access surface and the runtime
    // makes ZERO calls, which is exactly why adding it needs no
    // interface-version bump. See custom_browser/browser/nexus/remote_access/.
    "remoteAccess",
};

// Additive native primitives this host serves, by `<capability>.<action>`
// (or a named variant: `fs.writeFile.ifMatch`). Each one shipped on interface
// 1 without a bump (internal/docs/P-platform/P-3-bridge/native-primitives.md):
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

base::DictValue BuildNexusHostManifest(base::DictValue agent_config) {
  base::DictValue manifest;
  manifest.Set("hostKind", kHostKind);

  base::ListValue capabilities = BuildStringList(kBridgeCapabilities);
  // Phase 12g: dropped the `process` capability advertisement.
  // The bridge surface had no JS-side callers (Phase 11b deleted the
  // only consumers — TS BashTool / GrepTool / GlobTool). The C++
  // ProcessTool calls into NexusShellService directly via its own
  // launcher; nothing in the bridge serializes those calls.
  manifest.Set("capabilities", std::move(capabilities));

  // The additive primitives (see kNativePrimitives).
  manifest.Set("primitives", BuildStringList(kNativePrimitives));

  // The LLM bridge accepts a `document` part at the top level of a message
  // (nexus_llm.mojom kDocument), not only inside a tool result. The guest
  // inlines an @-mentioned PDF only when this is set, so an older binary —
  // which would reject the part and fail the turn — gets a pdf_reference
  // note instead. That is why it needs no interface-version bump.
  manifest.Set("llmDocumentParts", true);

  // Boot snapshot of the unified agent runtime config (maxTurns,
  // stepTimeoutSec, model, …). The guest applies this once after building
  // the runtime; later changes arrive live via `session.applyAgentConfig`.
  manifest.Set("agentConfig", std::move(agent_config));

  // Phase 12c: hostedTools used to live here. They now flow through the
  // `catalog` capability — JS calls `nexus.catalog.listHostedTools`
  // post-bootstrap to fetch the same array.
  return manifest;
}

std::string BuildNexusBridgeBootstrapMessage(base::DictValue agent_config) {
  base::DictValue message;
  message.Set("type", "nexus-bridge-bootstrap");
  message.Set("nexus", true);
  message.Set("manifest", BuildNexusHostManifest(std::move(agent_config)));
  return bridge_support::SerializeJson(base::Value(std::move(message)));
}

}  // namespace nexus
