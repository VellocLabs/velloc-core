#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_BRIDGE_BOOTSTRAP_HOST_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_BRIDGE_BOOTSTRAP_HOST_H_

#include <memory>

#include "base/memory/raw_ptr.h"
#include "custom_browser/browser/nexus/mojom/nexus_bridge_bootstrap.mojom.h"

class Profile;

namespace content {
class RenderFrameHost;
}

namespace nexus {

class NexusBridgePortService;

class NexusBridgeBootstrapHost : public mojom::NexusBridgeBootstrap {
 public:
  NexusBridgeBootstrapHost(content::RenderFrameHost* render_frame_host,
                           Profile* profile);
  ~NexusBridgeBootstrapHost() override;

  NexusBridgeBootstrapHost(const NexusBridgeBootstrapHost&) = delete;
  NexusBridgeBootstrapHost& operator=(const NexusBridgeBootstrapHost&) = delete;

  void GuestReady() override;

 private:
  raw_ptr<content::RenderFrameHost> render_frame_host_;
  raw_ptr<Profile> profile_;
  std::unique_ptr<NexusBridgePortService> page_bridge_;
  std::unique_ptr<NexusBridgePortService> worker_bridge_;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_BRIDGE_BOOTSTRAP_HOST_H_
