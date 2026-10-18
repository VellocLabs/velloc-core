#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_HOST_MANIFEST_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_HOST_MANIFEST_H_

#include <string>

#include "base/values.h"

namespace nexus {

// What this host can do beyond the static primitive list.
struct NexusHostFeatures {
  // The shell service can run a child under a pseudoconsole, so
  // `shell.spawn.pty` is listed (F-31).
  bool pty_supported = false;
};

// Builds the bootstrap manifest `{hostKind, capabilities, agentConfig}` shipped
// to the guest. `agentConfig` is a boot snapshot; later changes arrive via the
// `session.applyAgentConfig` event. It carries no tool definitions.
base::DictValue BuildNexusHostManifest(base::DictValue agent_config,
                                       const NexusHostFeatures& features);

// Convenience wrapper that serializes the bootstrap message envelope
// containing the manifest. Posted to the guest frame via
// `MessagePortProvider::PostMessageToFrame` together with two
// transferred ports (page + worker bridges).
std::string BuildNexusBridgeBootstrapMessage(base::DictValue agent_config,
                                             const NexusHostFeatures& features);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_HOST_MANIFEST_H_
