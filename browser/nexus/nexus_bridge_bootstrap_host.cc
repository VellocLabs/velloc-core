#include "custom_browser/browser/nexus/nexus_bridge_bootstrap_host.h"

#include <memory>
#include <utility>
#include <vector>

#include "base/logging.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/message_port_provider.h"
#include "content/public/browser/render_frame_host.h"
#include "custom_browser/browser/nexus/nexus_bridge_port_service.h"
#include "custom_browser/browser/nexus/nexus_bridge_support.h"
#include "custom_browser/browser/nexus/nexus_browser_tools_config.h"
#include "custom_browser/browser/nexus/nexus_host_manifest.h"
#include "url/origin.h"

namespace nexus {

NexusBridgeBootstrapHost::NexusBridgeBootstrapHost(
    content::RenderFrameHost* render_frame_host,
    Profile* profile)
    : render_frame_host_(render_frame_host), profile_(profile) {}

NexusBridgeBootstrapHost::~NexusBridgeBootstrapHost() = default;

void NexusBridgeBootstrapHost::GuestReady() {
  if (!render_frame_host_ || !profile_) {
    LOG(WARNING) << "[NexusTrace][BootstrapHost] GuestReady skipped: missing "
                 << "render frame host or profile";
    return;
  }

  LOG(INFO) << "[NexusTrace][BootstrapHost] GuestReady from frame "
            << render_frame_host_->GetLastCommittedURL();

  if (!page_bridge_) {
    page_bridge_ = std::make_unique<NexusBridgePortService>(profile_);
  }
  if (!worker_bridge_) {
    worker_bridge_ = std::make_unique<NexusBridgePortService>(profile_);
  }

  blink::MessagePortDescriptor page_port = page_bridge_->CreatePortPair();
  blink::MessagePortDescriptor worker_port = worker_bridge_->CreatePortPair();

  std::vector<blink::MessagePortDescriptor> ports;
  ports.push_back(std::move(page_port));
  ports.push_back(std::move(worker_port));

  const url::Origin host_origin =
      url::Origin::Create(render_frame_host_->GetLastCommittedURL());

  // Phase 12c: hostedTools no longer ride the bootstrap message.
  // The JS guest pulls them post-bootstrap via the `catalog` bridge
  // capability (`nexus.catalog.listHostedTools`). Bootstrap stays
  // minimal — capabilities + transferred ports.
  content::MessagePortProvider::PostMessageToFrame(
      render_frame_host_->GetPage(), &host_origin, &host_origin,
      bridge_support::Utf8Payload(
          BuildNexusBridgeBootstrapMessage(GetAgentConfig(profile_))),
      std::move(ports));
  LOG(INFO) << "[NexusTrace][BootstrapHost] Posted bootstrap message + 2 "
            << "ports to host page (hostedTools fetched via catalog API)";
}

}  // namespace nexus
