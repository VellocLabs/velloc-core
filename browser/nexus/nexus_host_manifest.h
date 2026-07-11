#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_HOST_MANIFEST_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_HOST_MANIFEST_H_

#include <string>

#include "base/containers/span.h"
#include "base/values.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_schema.h"

namespace nexus {

// Builds the bootstrap manifest dict the C++ side ships to the guest.
// `hosted_tools` is typically `ToolCatalog::Get().schemas()`; the span
// is converted into the wire-format `hostedTools` array consumed by
// the JS guest's `getToolsForHostManifest()` (`nexus_web/src/tools.ts`).
// An empty span omits the wire field; the JS guest then falls back to
// its locally-defined catalog (the Phase 1.5 stop-gap).
base::DictValue BuildNexusHostManifest(
    base::span<const ToolSchema> hosted_tools = {});

// Convenience wrapper that serializes the bootstrap message envelope
// containing the manifest. Kept for callers that don't have access to a
// schema list and only need the legacy minimal manifest.
std::string BuildNexusBridgeBootstrapMessage();

// Same as above but includes the hosted-tool catalog in the manifest.
// Preferred over the no-arg version once a `NexusBridgePortService` is
// in scope (it is in `nexus_bridge_bootstrap_host.cc`).
std::string BuildNexusBridgeBootstrapMessageWithTools(
    base::span<const ToolSchema> hosted_tools);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_HOST_MANIFEST_H_
