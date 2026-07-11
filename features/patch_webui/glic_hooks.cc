#include "custom_browser/features/patch_webui/glic_hooks.h"

#include <string_view>

#include "chrome/browser/profiles/profile.h"
#include "chrome/common/webui_url_constants.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/browser/render_frame_host.h"
#include "custom_browser/browser/nexus/nexus_guest_config.h"
#include "custom_browser/browser/nexus/nexus_bridge_bootstrap_host.h"
#include "custom_browser/browser/nexus/mojom/nexus_bridge_bootstrap.mojom.h"
#include "custom_browser/features/patch_webui/patch_resources.h"
#include "mojo/public/cpp/bindings/generic_pending_receiver.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"

namespace custom_browser::patch_webui {

namespace {

GURL GetCurrentWebUiUrl(content::WebContents* web_contents) {
  CHECK(web_contents);
  auto& controller = web_contents->GetController();
  if (const auto* pending_entry = controller.GetPendingEntry()) {
    const GURL& pending_url = pending_entry->GetVirtualURL();
    if (pending_url.is_valid() && !pending_url.is_empty()) {
      return pending_url;
    }
  }

  const GURL& visible_url = web_contents->GetVisibleURL();
  if (visible_url.is_valid() && !visible_url.is_empty()) {
    return visible_url;
  }

  if (const auto* visible_entry = controller.GetVisibleEntry()) {
    const GURL& visible_entry_url = visible_entry->GetVirtualURL();
    if (visible_entry_url.is_valid() && !visible_entry_url.is_empty()) {
      return visible_entry_url;
    }
  }

  if (const auto* last_committed_entry = controller.GetLastCommittedEntry()) {
    const GURL& committed_url = last_committed_entry->GetVirtualURL();
    if (committed_url.is_valid() && !committed_url.is_empty()) {
      return committed_url;
    }
  }

  return web_contents->GetLastCommittedURL();
}

GURL GetCurrentWebUiUrl(content::RenderFrameHost* render_frame_host) {
  CHECK(render_frame_host);
  const GURL& frame_url = render_frame_host->GetLastCommittedURL();
  if (frame_url.is_valid() && !frame_url.is_empty()) {
    return frame_url;
  }

  content::WebUI* web_ui = render_frame_host->GetWebUI();
  if (!web_ui) {
    return GURL();
  }

  auto* web_contents = web_ui->GetWebContents();
  if (!web_contents) {
    return GURL();
  }

  return GetCurrentWebUiUrl(web_contents);
}

bool IsNexusGlicRenderFrame(content::RenderFrameHost* render_frame_host) {
  if (!render_frame_host) {
    return false;
  }

  return nexus::IsNexusGlicMode(GetCurrentWebUiUrl(render_frame_host));
}

}  // namespace

void OnGlicDataSourceConfiguring(content::WebUI* web_ui,
                                 content::WebUIDataSource* data_source) {
  auto* web_contents = web_ui->GetWebContents();
  AddPatchResourcesForHost(chrome::kChromeUIGlicHost, data_source);

  auto* profile =
      Profile::FromBrowserContext(web_contents->GetBrowserContext());
  const bool is_nexus_mode = nexus::IsNexusGlicMode(GetCurrentWebUiUrl(web_contents));

  if (is_nexus_mode) {
    const GURL guest_url = nexus::GetResolvedNexusGuestURL(profile);
    data_source->AddString("glicGuestURL", guest_url.spec());
    // Override FRE check so Nexus WebUI bypasses it entirely.
    data_source->AddBoolean("shouldShowFre", false);
  }
  data_source->AddBoolean("isNexusMode", is_nexus_mode);
}

bool BindNexusBridgeBootstrapForGlic(
    content::RenderFrameHost* render_frame_host,
    mojo::GenericPendingReceiver* receiver) {
  if (!render_frame_host || !receiver || !receiver->is_valid()) {
    return false;
  }

  if (!IsNexusGlicRenderFrame(render_frame_host)) {
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
