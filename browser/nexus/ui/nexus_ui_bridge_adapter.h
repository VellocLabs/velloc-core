#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_UI_NEXUS_UI_BRIDGE_ADAPTER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_UI_NEXUS_UI_BRIDGE_ADAPTER_H_

#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "custom_browser/browser/nexus/nexus_bridge_capability_handler.h"

class Profile;

namespace nexus {

class NexusBridgeResponseDelegate;

class NexusUiBridgeAdapter : public NexusBridgeCapabilityHandler {
 public:
  NexusUiBridgeAdapter(NexusBridgeResponseDelegate* delegate, Profile* profile);
  ~NexusUiBridgeAdapter() override;

  NexusUiBridgeAdapter(const NexusUiBridgeAdapter&) = delete;
  NexusUiBridgeAdapter& operator=(const NexusUiBridgeAdapter&) = delete;

 private:
  void HandleOpenNexusSettings(
      const std::string& id,
      const base::DictValue& route,
      const base::DictValue& payload,
      std::vector<blink::MessagePortDescriptor> ports);

  void HandleGetProviderMissingPrompt(
      const std::string& id,
      const base::DictValue& route,
      const base::DictValue& payload,
      std::vector<blink::MessagePortDescriptor> ports);

  raw_ptr<Profile> profile_;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_UI_NEXUS_UI_BRIDGE_ADAPTER_H_
