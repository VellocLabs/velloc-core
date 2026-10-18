#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_BRIDGE_BOOTSTRAP_HOST_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_BRIDGE_BOOTSTRAP_HOST_H_

#include <memory>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "content/public/browser/weak_document_ptr.h"
#include "custom_browser/browser/nexus/mojom/nexus_bridge_bootstrap.mojom.h"
#include "custom_browser/browser/nexus/nexus_host_manifest.h"

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
  // Approach 5 (A5): lazily creates `panel_bridge_` and posts a
  // `nexus-panel-bridge-attach` message + 1 transferred port back to the
  // calling frame's window. Idempotent — each call resets the existing port
  // and creates a fresh one, so a panel reload or a worker-side reconnect
  // can re-attach safely without leaving stale ports behind.
  void AttachPanelBridge() override;

  // True once the guest bootstrap has been posted to the frame.
  bool bootstrap_posted_for_testing() const { return !!page_bridge_; }

 private:
  // The frame of `document_` while that document is live and active, else
  // null.
  content::RenderFrameHost* ActiveFrame() const;

  // Second half of GuestReady, once the host features are known: creates
  // the port pairs and posts the bootstrap message to the frame. Skipped if
  // the document that asked is gone or inactive (frame closed, navigated or
  // cached) in between.
  void PostBootstrap(NexusHostFeatures features);

  // The document this host was bound for. Every entry point resolves it
  // afresh: the receiver that owns this host outlives the frame, so a raw
  // RenderFrameHost pointer could dangle.
  content::WeakDocumentPtr document_;
  raw_ptr<Profile> profile_;
  std::unique_ptr<NexusBridgePortService> page_bridge_;
  std::unique_ptr<NexusBridgePortService> worker_bridge_;
  // A5: separate port service for the WebUI panel itself
  // (chrome://custom.top-chrome). Distinct lifetime + capability set from
  // the guest-facing page/worker bridges — the panel only needs the
  // settings-related capabilities (initially: `providers`), so its
  // dispatcher is constructed with `kPanel` mode.
  std::unique_ptr<NexusBridgePortService> panel_bridge_;

  base::WeakPtrFactory<NexusBridgeBootstrapHost> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_BRIDGE_BOOTSTRAP_HOST_H_
