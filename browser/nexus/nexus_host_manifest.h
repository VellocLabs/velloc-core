#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_HOST_MANIFEST_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_HOST_MANIFEST_H_

#include <string>

#include "base/values.h"

namespace nexus {

// Builds the bootstrap manifest dict the C++ side ships to the guest.
//
// Carries `{hostKind, capabilities, agentConfig}`. `agentConfig` is the
// boot snapshot of the unified agent runtime config (see GetAgentConfig);
// the guest applies it once after building the runtime, and subsequent
// changes arrive live via the `session.applyAgentConfig` event.
//
// The manifest carries no tool definitions: the runtime defines the host
// tools itself and offers them by the advertised capabilities.
base::DictValue BuildNexusHostManifest(base::DictValue agent_config);

// Convenience wrapper that serializes the bootstrap message envelope
// containing the manifest. Posted to the guest frame via
// `MessagePortProvider::PostMessageToFrame` together with two
// transferred ports (page + worker bridges).
std::string BuildNexusBridgeBootstrapMessage(base::DictValue agent_config);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_HOST_MANIFEST_H_
