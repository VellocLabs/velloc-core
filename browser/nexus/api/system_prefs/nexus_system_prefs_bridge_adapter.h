#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_SYSTEM_PREFS_NEXUS_SYSTEM_PREFS_BRIDGE_ADAPTER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_SYSTEM_PREFS_NEXUS_SYSTEM_PREFS_BRIDGE_ADAPTER_H_

#include <vector>

#include "custom_browser/browser/nexus/api/system_prefs/system_prefs_api.gen.h"
#include "custom_browser/browser/nexus/bridge_response_callback.h"

class Profile;

namespace blink {
class MessagePortDescriptor;
}

namespace nexus {

class NexusBridgeResponseDelegate;

// Concrete adapter implementing the generated `SystemPrefsApiBase` virtuals:
// browser-global prefs in `local_state()` for the Settings · System page.
// All actions run on UI; no per-call state and no profile.
class NexusSystemPrefsBridgeAdapter
    : public ::nexus::generated::SystemPrefsApiBase {
 public:
  NexusSystemPrefsBridgeAdapter(NexusBridgeResponseDelegate* delegate,
                                Profile* profile);
  ~NexusSystemPrefsBridgeAdapter() override;

  NexusSystemPrefsBridgeAdapter(const NexusSystemPrefsBridgeAdapter&) = delete;
  NexusSystemPrefsBridgeAdapter& operator=(
      const NexusSystemPrefsBridgeAdapter&) = delete;

 private:
  // Read both prefs. Returns `{backgroundModeEnabled, launcherEnabled}`.
  void HandleGetOnUI(::nexus::generated::GetInput input,
                     std::vector<blink::MessagePortDescriptor> ports,
                     ::nexus::BridgeResponseCallback done) override;

  // Write `prefs::kBackgroundModeEnabled` on local_state. Returns `{ok:true}`.
  void HandleSetBackgroundModeOnUI(
      ::nexus::generated::SetBackgroundModeInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  // Alias of HandleSetBackgroundModeOnUI: writes `prefs::kBackgroundModeEnabled`
  // (the toggles were merged; kept for wire compatibility).
  // Returns `{ok:true}`.
  void HandleSetLauncherOnUI(
      ::nexus::generated::SetLauncherInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_SYSTEM_PREFS_NEXUS_SYSTEM_PREFS_BRIDGE_ADAPTER_H_
