#include "custom_browser/browser/nexus/nexus_host_manifest.h"

#include <array>

#include "base/values.h"
#include "custom_browser/browser/nexus/nexus_bridge_support.h"
#include "custom_browser/browser/nexus/nexus_features.h"

namespace nexus {

namespace {

constexpr char kHostKind[] = "nexus";

constexpr std::array<const char*, 4> kBridgeCapabilities = {
    "fs",
    "model",
    "glic",
    "ui",
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

base::DictValue BuildNexusHostManifest() {
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
  return manifest;
}

std::string BuildNexusBridgeBootstrapMessage() {
  base::DictValue message;
  message.Set("type", "nexus-bridge-bootstrap");
  message.Set("nexus", true);
  message.Set("manifest", BuildNexusHostManifest());
  return bridge_support::SerializeJson(base::Value(std::move(message)));
}

}  // namespace nexus
