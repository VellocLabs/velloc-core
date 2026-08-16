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

// Concrete adapter implementing the generated `SearchApiBase` virtual.
//
// The adapter is a forwarder and nothing else. Searching a transcript —
// mmap, span indexing, matching — and the `.sdi` sidecar reads and writes
// that go with it all happen in a utility process; the browser process
// sends an ordered list of transcript paths and receives matches. Keeping
// it out of process is what lets the sidecar be maintained lazily on
// search without adding a single line to the transcript write path, and
// it means a malformed transcript can at worst kill a child process.
//
// The remote is shared per profile (`GetSharedSearchLauncher`), so every
// bridge port searches through one process with one warm span cache.
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
