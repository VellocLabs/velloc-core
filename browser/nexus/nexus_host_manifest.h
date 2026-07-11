#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_HOST_MANIFEST_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_HOST_MANIFEST_H_

#include <string>

#include "base/values.h"

namespace nexus {

// Builds the bootstrap manifest dict the C++ side ships to the guest.
//
// Phase 12c: the manifest carries ONLY `{hostKind, capabilities}`. The
// pre-Phase-12c `hostedTools` field has moved to the `catalog` bridge
// capability — the JS guest pulls them via
// `nexus.catalog.listHostedTools` after the bootstrap port is alive.
// See custom_browser/browser/nexus/api/catalog/.
base::DictValue BuildNexusHostManifest();

// Convenience wrapper that serializes the bootstrap message envelope
// containing the manifest. Posted to the guest frame via
// `MessagePortProvider::PostMessageToFrame` together with two
// transferred ports (page + worker bridges).
std::string BuildNexusBridgeBootstrapMessage();

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_HOST_MANIFEST_H_
