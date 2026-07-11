#include "custom_browser/browser/nexus/nexus_host_manifest.h"

#include <array>
#include <utility>

#include "base/values.h"
#include "custom_browser/browser/nexus/nexus_bridge_support.h"
#include "custom_browser/browser/nexus/nexus_features.h"

namespace nexus {

namespace {

constexpr char kHostKind[] = "nexus";

constexpr std::array<const char*, 4> kBridgeCapabilities = {
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
    // `browser.<tool_name>` and per-session lifecycle uses
    // `browser.endSession`.
    "browser",
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

base::DictValue BuildNexusHostManifest(
    base::span<const ToolSchema> hosted_tools) {
  base::DictValue manifest;
  manifest.Set("hostKind", kHostKind);

  base::ListValue capabilities = BuildStringList(kBridgeCapabilities);
  // The `process` capability is gated behind PrereleaseNexusProcessCapability
  // — only advertise it when the bridge will actually register the
  // adapter (see NexusBridgePortService). Both surfaces must agree:
  // advertising "process" here without registering the adapter would
  // make every `process.*` call hang waiting for a handler.
  if (features::IsNexusProcessCapabilityEnabled()) {
    capabilities.Append("process");
  }
  manifest.Set("capabilities", std::move(capabilities));

  // Emit the catalog the LLM-facing JS reads as `manifest.hostedTools`.
  // Skip the field entirely if empty so the JS-side fallback path
  // (`getToolsForHostManifest` in `nexus_web/src/tools.ts`) is exercised
  // unchanged when no C++ tools are advertised.
  if (!hosted_tools.empty()) {
    manifest.Set("hostedTools", BuildHostedToolsList(hosted_tools));
  }
  return manifest;
}

std::string BuildNexusBridgeBootstrapMessage() {
  base::DictValue message;
  message.Set("type", "nexus-bridge-bootstrap");
  message.Set("nexus", true);
  message.Set("manifest", BuildNexusHostManifest());
  return bridge_support::SerializeJson(base::Value(std::move(message)));
}

std::string BuildNexusBridgeBootstrapMessageWithTools(
    base::span<const ToolSchema> hosted_tools) {
  base::DictValue message;
  message.Set("type", "nexus-bridge-bootstrap");
  message.Set("nexus", true);
  message.Set("manifest", BuildNexusHostManifest(hosted_tools));
  return bridge_support::SerializeJson(base::Value(std::move(message)));
}

}  // namespace nexus
