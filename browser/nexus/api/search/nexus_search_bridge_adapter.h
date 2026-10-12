#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_SEARCH_NEXUS_SEARCH_BRIDGE_ADAPTER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_SEARCH_NEXUS_SEARCH_BRIDGE_ADAPTER_H_

#include <vector>

#include "base/memory/raw_ptr.h"
#include "custom_browser/browser/nexus/api/search/search_api.gen.h"
#include "custom_browser/browser/nexus/bridge_response_callback.h"

class Profile;

namespace blink {
class MessagePortDescriptor;
}

namespace nexus {

class NexusBridgeResponseDelegate;

// Concrete adapter implementing the generated `SearchApiBase` virtual: a pure
// forwarder to the per-profile search utility process, which owns transcript
// matching and the `.sdi` sidecars.
class NexusSearchBridgeAdapter : public ::nexus::generated::SearchApiBase {
 public:
  NexusSearchBridgeAdapter(NexusBridgeResponseDelegate* delegate,
                           Profile* profile);
  ~NexusSearchBridgeAdapter() override;

  NexusSearchBridgeAdapter(const NexusSearchBridgeAdapter&) = delete;
  NexusSearchBridgeAdapter& operator=(const NexusSearchBridgeAdapter&) = delete;

 private:
  void HandleQueryTranscriptsOnUI(
      ::nexus::generated::QueryTranscriptsInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  raw_ptr<Profile> profile_;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_SEARCH_NEXUS_SEARCH_BRIDGE_ADAPTER_H_
