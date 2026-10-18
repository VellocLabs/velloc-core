#include "custom_browser/browser/nexus/nexus_bridge_bootstrap_host.h"

#include <memory>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/task/thread_pool.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/message_port_provider.h"
#include "content/public/browser/render_frame_host.h"
#include "custom_browser/browser/nexus/nexus_bridge_port_service.h"
#include "custom_browser/browser/nexus/nexus_bridge_support.h"
#include "custom_browser/browser/nexus/nexus_browser_tools_config.h"
#include "custom_browser/browser/nexus/nexus_host_manifest.h"
#include "custom_browser/utility/nexus/shell/pty_console_host.h"
#include "url/origin.h"

namespace nexus {

namespace {

// A5: the JSON envelope returned to the WebUI panel when it calls
// `AttachPanelBridge()`. Mirrors the bootstrap envelope shape used by
// `BuildNexusBridgeBootstrapMessage` so the panel-side listener uses the
// same parse-then-dispatch-on-type pattern. `type` is the only thing the
// panel needs to read; it identifies which message it's seeing
// (`nexus-bridge-bootstrap` is forwarded to the webview;
// `nexus-panel-bridge-attach` is consumed by the panel itself).
std::string BuildPanelBridgeAttachMessage() {
  base::DictValue payload;
  payload.Set("type", "nexus-panel-bridge-attach");
  payload.Set("nexus", true);
  std::string serialized;
  base::JSONWriter::Write(payload, &serialized);
  return serialized;
}

// Blocking: probes the console host (file checks, LoadLibrary), so it runs
// on the thread pool. The same resolver the shell service uses, so the
// manifest and `GetCapabilities` agree (F-31).
NexusHostFeatures ProbeHostFeatures() {
  NexusHostFeatures features;
  features.pty_supported = PtyConsoleHost::Get().available();
  return features;
}

}  // namespace

NexusBridgeBootstrapHost::NexusBridgeBootstrapHost(
    content::RenderFrameHost* render_frame_host,
    Profile* profile)
    : document_(render_frame_host->GetWeakDocumentPtr()), profile_(profile) {}

NexusBridgeBootstrapHost::~NexusBridgeBootstrapHost() = default;

content::RenderFrameHost* NexusBridgeBootstrapHost::ActiveFrame() const {
  // A document in the back-forward cache is still a live frame, but not the
  // one the person sees: ports posted there would serve a hidden page.
  content::RenderFrameHost* render_frame_host =
      document_.AsRenderFrameHostIfValid();
  return render_frame_host && render_frame_host->IsActive() ? render_frame_host
                                                            : nullptr;
}

void NexusBridgeBootstrapHost::GuestReady() {
  content::RenderFrameHost* render_frame_host = ActiveFrame();
  if (!render_frame_host || !profile_) {
    LOG(WARNING) << "[NexusTrace][BootstrapHost] GuestReady skipped: missing "
                 << "render frame host or profile";
    return;
  }

  LOG(INFO) << "[NexusTrace][BootstrapHost] GuestReady from frame "
            << render_frame_host->GetLastCommittedURL();

  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock()}, base::BindOnce(&ProbeHostFeatures),
      base::BindOnce(&NexusBridgeBootstrapHost::PostBootstrap,
                     weak_factory_.GetWeakPtr()));
}

void NexusBridgeBootstrapHost::PostBootstrap(NexusHostFeatures features) {
  content::RenderFrameHost* render_frame_host = ActiveFrame();
  if (!render_frame_host) {
    LOG(WARNING) << "[NexusTrace][BootstrapHost] bootstrap skipped: document "
                 << "gone or inactive";
    return;
  }

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
      url::Origin::Create(render_frame_host->GetLastCommittedURL());

  // The bootstrap message carries no tool definitions: the runtime defines
  // the host tools itself and offers them by the advertised capabilities.
  // Bootstrap stays minimal — capabilities + transferred ports.
  content::MessagePortProvider::PostMessageToFrame(
      render_frame_host->GetPage(), &host_origin, &host_origin,
      bridge_support::Utf8Payload(
          BuildNexusBridgeBootstrapMessage(GetAgentConfig(profile_),
                                           features)),
      std::move(ports));
  LOG(INFO) << "[NexusTrace][BootstrapHost] Posted bootstrap message + 2 "
            << "ports to host page (hostedTools fetched via catalog API)";
}

void NexusBridgeBootstrapHost::AttachPanelBridge() {
  content::RenderFrameHost* render_frame_host = ActiveFrame();
  if (!render_frame_host || !profile_) {
    LOG(WARNING) << "[NexusTrace][BootstrapHost] AttachPanelBridge skipped: "
                 << "missing render frame host or profile";
    return;
  }

  LOG(INFO) << "[NexusTrace][BootstrapHost] AttachPanelBridge from frame "
            << render_frame_host->GetLastCommittedURL();

  // Idempotent: lazily create the panel port service; CreatePortPair()
  // already resets any previously-allocated port. This is what makes the
  // attach handshake replayable — a panel reload (Ctrl+R) can call this
  // again and the C++ side drops the dead port cleanly before handing back
  // a fresh one.
  if (!panel_bridge_) {
    panel_bridge_ = std::make_unique<NexusBridgePortService>(
        profile_, PortServiceMode::kPanel);
  }

  blink::MessagePortDescriptor panel_port = panel_bridge_->CreatePortPair();

  std::vector<blink::MessagePortDescriptor> ports;
  ports.push_back(std::move(panel_port));

  const url::Origin host_origin =
      url::Origin::Create(render_frame_host->GetLastCommittedURL());

  // Single envelope, single port. Same `PostMessageToFrame` mechanism the
  // guest bootstrap uses — keeps the WebUI's message-handling shape
  // uniform (parse + dispatch on `type`, transferred ports in
  // `event.ports`).
  content::MessagePortProvider::PostMessageToFrame(
      render_frame_host->GetPage(), &host_origin, &host_origin,
      bridge_support::Utf8Payload(BuildPanelBridgeAttachMessage()),
      std::move(ports));
  LOG(INFO) << "[NexusTrace][BootstrapHost] Posted nexus-panel-bridge-attach "
            << "+ 1 port to host page";
}

}  // namespace nexus
