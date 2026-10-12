#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_APP_INFO_NEXUS_APP_INFO_BRIDGE_ADAPTER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_APP_INFO_NEXUS_APP_INFO_BRIDGE_ADAPTER_H_

#include <vector>

#include "base/memory/raw_ptr.h"
#include "custom_browser/browser/nexus/api/app_info/app_info_api.gen.h"
#include "custom_browser/browser/nexus/bridge_response_callback.h"

class Profile;

namespace blink {
class MessagePortDescriptor;
}

namespace nexus {

class NexusBridgeResponseDelegate;

// Concrete adapter implementing the generated `AppInfoApiBase` virtuals:
// version strings, the custom webapp URL (`nexus.webapp_url` profile pref) and
// the `relaunch` that applies it. All actions run on UI.
class NexusAppInfoBridgeAdapter
    : public ::nexus::generated::AppInfoApiBase {
 public:
  NexusAppInfoBridgeAdapter(NexusBridgeResponseDelegate* delegate,
                            Profile* profile);
  ~NexusAppInfoBridgeAdapter() override;

  NexusAppInfoBridgeAdapter(const NexusAppInfoBridgeAdapter&) = delete;
  NexusAppInfoBridgeAdapter& operator=(const NexusAppInfoBridgeAdapter&) =
      delete;

 private:
  // Returns `{appVersion, chromiumVersion}`.
  void HandleGetVersionInfoOnUI(
      ::nexus::generated::GetVersionInfoInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  // Returns `{url}` from the `nexus.webapp_url` profile pref.
  void HandleGetWebappUrlOnUI(
      ::nexus::generated::GetWebappUrlInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  // Writes the `nexus.webapp_url` profile pref. Returns `{ok:true}`.
  void HandleSetWebappUrlOnUI(
      ::nexus::generated::SetWebappUrlInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  // Restarts the client via `chrome::AttemptRestart()`. Replies `{ok:true}`
  // first, then posts the restart — see the .cc for why the order matters.
  void HandleRelaunchOnUI(
      ::nexus::generated::RelaunchInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  raw_ptr<Profile> profile_;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_APP_INFO_NEXUS_APP_INFO_BRIDGE_ADAPTER_H_
