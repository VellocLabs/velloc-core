#include "custom_browser/features/patch_webui/nexus_bootstrap_hooks.h"

#include "chrome/browser/profiles/profile.h"
#include "chrome/common/webui_url_constants.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/common/url_constants.h"
#include "custom_browser/browser/nexus/nexus_bridge_bootstrap_host.h"
#include "custom_browser/browser/nexus/mojom/nexus_bridge_bootstrap.mojom.h"
#include "mojo/public/cpp/bindings/generic_pending_receiver.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"

namespace custom_browser::patch_webui {

namespace {

// True for the chrome://custom.top-chrome WebUI frame that hosts the Nexus
// guest <webview> directly. This is the only bootstrap target now that the
// chrome://glic intermediate page has been removed.
bool IsCustomBrowserRenderFrame(content::RenderFrameHost* render_frame_host) {
  if (!render_frame_host) {
    return false;
  }
  const GURL& url = render_frame_host->GetLastCommittedURL();
  return url.is_valid() && url.SchemeIs(content::kChromeUIScheme) &&
         url.host() == chrome::kChromeUICustomBrowserHost;
}

}  // namespace

bool BindNexusBridgeBootstrap(content::RenderFrameHost* render_frame_host,
                              mojo::GenericPendingReceiver* receiver) {
  if (!render_frame_host || !receiver || !receiver->is_valid()) {
    return false;
  }

  if (!IsCustomBrowserRenderFrame(render_frame_host)) {
    return false;
  }

  auto* profile =
      Profile::FromBrowserContext(render_frame_host->GetBrowserContext());
  if (!profile) {
    return false;
  }

  auto bootstrap_receiver =
      receiver->As<nexus::mojom::NexusBridgeBootstrap>();
  if (bootstrap_receiver.is_valid()) {
    mojo::MakeSelfOwnedReceiver(
        std::make_unique<nexus::NexusBridgeBootstrapHost>(render_frame_host,
                                                          profile),
        std::move(bootstrap_receiver));
    return true;
  }

  return false;
}

}  // namespace custom_browser::patch_webui
