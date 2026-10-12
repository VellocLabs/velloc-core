#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_BROWSER_TOOLS_NEXUS_BROWSER_TOOLS_BRIDGE_ADAPTER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_BROWSER_TOOLS_NEXUS_BROWSER_TOOLS_BRIDGE_ADAPTER_H_

#include <vector>

#include "base/memory/raw_ptr.h"
#include "custom_browser/browser/nexus/api/browser_tools/browser_tools_api.gen.h"
#include "custom_browser/browser/nexus/bridge_response_callback.h"

class Profile;

namespace blink {
class MessagePortDescriptor;
}

namespace nexus {

class NexusBridgeResponseDelegate;

// Concrete adapter implementing the generated `BrowserToolsApiBase` virtuals.
// Exposed only on the panel WebUI port (`PortServiceMode::kPanel`);
// `separateWindow` is the only field of its `snapshot` / `setField` schema.
class NexusBrowserToolsBridgeAdapter
    : public ::nexus::generated::BrowserToolsApiBase {
 public:
  NexusBrowserToolsBridgeAdapter(NexusBridgeResponseDelegate* delegate,
                                 Profile* profile);
  ~NexusBrowserToolsBridgeAdapter() override;

  NexusBrowserToolsBridgeAdapter(const NexusBrowserToolsBridgeAdapter&) =
      delete;
  NexusBrowserToolsBridgeAdapter& operator=(
      const NexusBrowserToolsBridgeAdapter&) = delete;

 private:
  // Read effective settings. Returns `{values: {separateWindow}}`.
  void HandleSnapshotOnUI(
      ::nexus::generated::SnapshotInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  // Apply a sparse patch and return the post-write snapshot.
  // Returns `{ok: true, values: {separateWindow}}` on success.
  void HandleSetFieldOnUI(
      ::nexus::generated::SetFieldInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  raw_ptr<Profile> profile_;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_BROWSER_TOOLS_NEXUS_BROWSER_TOOLS_BRIDGE_ADAPTER_H_
