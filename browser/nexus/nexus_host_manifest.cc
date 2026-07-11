#include "custom_browser/browser/nexus/nexus_host_manifest.h"

#include <array>
#include <utility>

#include "base/values.h"
#include "custom_browser/browser/nexus/nexus_bridge_support.h"

namespace nexus {

namespace {

constexpr char kHostKind[] = "nexus";

constexpr std::array<const char*, 6> kBridgeCapabilities = {
    "fs",
    "model",
    "ui",
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
    // Phase 12f: dedicated session capability. `publishStatus` (cache
    // for the WebUI quick-settings popup), `onStop` (LLM-session
    // OnStop cascade — renamed from `browser.endSession`),
    // `setAgentLoopSettings` (per-session wall-clock budgets). See
    // custom_browser/browser/nexus/api/session/.
    "session",
    // Phase 12: catalog metadata served lazily over the bridge. JS
    // pulls `listHostedTools` (replacing the bootstrap manifest's
    // hostedTools field) and `renderPromptSection` (replacing TS-side
    // getUsingYourToolsSection rendering). See
    // custom_browser/browser/nexus/api/catalog/.
    "catalog",
};

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
