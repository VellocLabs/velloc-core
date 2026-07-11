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

// Concrete adapter implementing the generated `BrowserToolsApiBase`
// virtuals.
//
// Approach 5 (A5): exposed ONLY on the panel WebUI port
// (`PortServiceMode::kPanel`). Replaces the chrome.send
// `getBrowserToolsSeparateWindow` / `setBrowserToolsSeparateWindow`
// routes that backed the Settings · Tools toggle (and the topbar's
// one-shot read).
//
// Schema scope: `separateWindow` is the only field today. The schema
// is built as a single-field `snapshot` / `setField` pattern (mirrors
// the existing `agent` and `features` capabilities) so adding a future
// toggle is purely a matter of extending the dict — no protocol churn.
// `allowEvaluate` was removed entirely (see
// nexus_browser_tools_config.h); BrowserEvaluate is now always
// available — a first-class browser tool with no opt-in gate.
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
