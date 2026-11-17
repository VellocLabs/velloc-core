// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/panel/nexus_app_page_frame_guard.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "components/network_hints/common/network_hints.mojom.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/navigation_throttle_registry.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "custom_browser/browser/nexus/panel/nexus_panel_guest.h"
#include "custom_browser/chromium_hooks/hook_registrar.h"
#include "mojo/public/cpp/bindings/binder_map.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/remote_set.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "net/base/ip_address.h"
#include "services/network/public/cpp/network_service_buildflags.h"
#include "url/gurl.h"
#include "url/scheme_host_port.h"

#if BUILDFLAG(IS_P2P_ENABLED)
#include "services/network/public/mojom/p2p.mojom.h"
#endif

namespace nexus {

namespace {

#if BUILDFLAG(IS_P2P_ENABLED)
// Contract: answers every call so the renderer neither waits nor reconnects.
// The network list is empty, a host name resolves to nothing, and a socket
// request has both its pipes closed unopened.
class RefusingP2PSocketManager : public network::mojom::P2PSocketManager {
 public:
  RefusingP2PSocketManager() = default;
  ~RefusingP2PSocketManager() override = default;

  // network::mojom::P2PSocketManager:
  void StartNetworkNotifications(
      mojo::PendingRemote<network::mojom::P2PNetworkNotificationClient> client)
      override {
    mojo::Remote<network::mojom::P2PNetworkNotificationClient> remote(
        std::move(client));
    remote->NetworkListChanged({}, net::IPAddress(), net::IPAddress());
    clients_.Add(std::move(remote));
  }

  void GetHostAddress(const std::string& host_name,
                      std::optional<net::AddressFamily> address_family,
                      bool enable_mdns,
                      GetHostAddressCallback callback) override {
    std::move(callback).Run({});
  }

  void CreateSocket(
      network::P2PSocketType type,
      const net::IPEndPoint& local_address,
      const network::P2PPortRange& port_range,
      const network::P2PHostAndIPEndPoint& remote_address,
      const net::MutableNetworkTrafficAnnotationTag& traffic_annotation,
      const std::optional<base::UnguessableToken>& devtools_token,
      mojo::PendingRemote<network::mojom::P2PSocketClient> client,
      mojo::PendingReceiver<network::mojom::P2PSocket> socket) override {}

 private:
  mojo::RemoteSet<network::mojom::P2PNetworkNotificationClient> clients_;
};

void BindRefusingP2PSocketManager(
    content::RenderFrameHost* frame,
    mojo::PendingReceiver<network::mojom::P2PSocketManager> receiver) {
  mojo::MakeSelfOwnedReceiver(std::make_unique<RefusingP2PSocketManager>(),
                              std::move(receiver));
}
#endif  // BUILDFLAG(IS_P2P_ENABLED)

class IgnoringNetworkHintsHandler
    : public network_hints::mojom::NetworkHintsHandler {
 public:
  IgnoringNetworkHintsHandler() = default;
  ~IgnoringNetworkHintsHandler() override = default;

  // network_hints::mojom::NetworkHintsHandler:
  void PrefetchDNS(const std::vector<url::SchemeHostPort>& url_list) override {}
  void Preconnect(const url::SchemeHostPort& url,
                  bool allow_credentials) override {}
};

void BindIgnoringNetworkHintsHandler(
    content::RenderFrameHost* frame,
    mojo::PendingReceiver<network_hints::mojom::NetworkHintsHandler>
        receiver) {
  mojo::MakeSelfOwnedReceiver(std::make_unique<IgnoringNetworkHintsHandler>(),
                              std::move(receiver));
}

void OverrideBindersFromHook(
    content::RenderFrameHost* frame,
    mojo::BinderMapWithContext<content::RenderFrameHost*>* map) {
  if (frame->GetParent() && frame->GetFrameName() == kAppPageFrameName) {
    AppPageFrames::Note(*frame,
                        IsNexusPanelGuestContents(
                            content::WebContents::FromRenderFrameHost(frame)));
  }
  OverrideAppPageFrameBinders(*frame, *map);
}

}  // namespace

bool IsAllowedAppPageFrameUrl(const GURL& url) {
  return url.IsAboutSrcdoc() || url.IsAboutBlank();
}

AppPageFrames::AppPageFrames(content::WebContents* contents)
    : content::WebContentsUserData<AppPageFrames>(*contents) {}

AppPageFrames::~AppPageFrames() = default;

// static
void AppPageFrames::Note(content::RenderFrameHost& frame, bool in_nexus_guest) {
  if (!in_nexus_guest || !frame.GetParent() ||
      frame.GetFrameName() != kAppPageFrameName) {
    return;
  }
  content::WebContents* contents =
      content::WebContents::FromRenderFrameHost(&frame);
  if (!contents) {
    return;
  }
  AppPageFrames::GetOrCreateForWebContents(contents)->latched_.insert(
      frame.GetFrameTreeNodeId());
}

// static
bool AppPageFrames::Contains(content::RenderFrameHost& frame) {
  const AppPageFrames* frames = AppPageFrames::FromWebContents(
      content::WebContents::FromRenderFrameHost(&frame));
  if (!frames) {
    return false;
  }
  for (content::RenderFrameHost* each = &frame; each;
       each = each->GetParent()) {
    if (frames->Has(each->GetFrameTreeNodeId())) {
      return true;
    }
  }
  return false;
}

// static
bool AppPageFrames::ContainsNavigation(content::NavigationHandle& handle) {
  if (handle.IsInMainFrame()) {
    return false;
  }
  const AppPageFrames* frames =
      AppPageFrames::FromWebContents(handle.GetWebContents());
  if (!frames) {
    return false;
  }
  if (frames->Has(handle.GetFrameTreeNodeId())) {
    return true;
  }
  content::RenderFrameHost* parent = handle.GetParentFrame();
  return parent && Contains(*parent);
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(AppPageFrames);

AppPageNavigationThrottle::AppPageNavigationThrottle(
    content::NavigationThrottleRegistry& registry)
    : content::NavigationThrottle(registry) {}

AppPageNavigationThrottle::~AppPageNavigationThrottle() = default;

content::NavigationThrottle::ThrottleCheckResult
AppPageNavigationThrottle::WillStartRequest() {
  return IsAllowedAppPageFrameUrl(navigation_handle()->GetURL())
             ? PROCEED
             : CANCEL_AND_IGNORE;
}

content::NavigationThrottle::ThrottleCheckResult
AppPageNavigationThrottle::WillRedirectRequest() {
  return CANCEL_AND_IGNORE;
}

const char* AppPageNavigationThrottle::GetNameForLogging() {
  return "NexusAppPageNavigationThrottle";
}

void MaybeAddAppPageNavigationThrottle(
    content::NavigationThrottleRegistry& registry) {
  if (!AppPageFrames::ContainsNavigation(registry.GetNavigationHandle())) {
    return;
  }
  registry.AddThrottle(std::make_unique<AppPageNavigationThrottle>(registry));
}

void OverrideAppPageFrameBinders(
    content::RenderFrameHost& frame,
    mojo::BinderMapWithContext<content::RenderFrameHost*>& map) {
  if (!AppPageFrames::Contains(frame)) {
    return;
  }
#if BUILDFLAG(IS_P2P_ENABLED)
  map.Add<network::mojom::P2PSocketManager>(&BindRefusingP2PSocketManager);
#endif
  map.Add<network_hints::mojom::NetworkHintsHandler>(
      &BindIgnoringNetworkHintsHandler);
}

void RegisterAppPageFrameGuardHooks(custom_browser::HookRegistrar* registrar) {
  registrar->AddNavigationThrottleProvider(
      base::BindRepeating(&MaybeAddAppPageNavigationThrottle));
  registrar->AddRenderFrameBinderOverride(
      base::BindRepeating(&OverrideBindersFromHook));
}

}  // namespace nexus
