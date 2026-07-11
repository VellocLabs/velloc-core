#include "custom_browser/browser/nexus/view/nexus_view_container.h"

#include "chrome/browser/glic/host/glic_ui.h"
#include "chrome/browser/glic/public/glic_keyed_service.h"
#include "chrome/browser/glic/public/glic_keyed_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents.h"
#include "url/gurl.h"

namespace {

constexpr char kNexusWebUiUrl[] = "chrome://glic/?openclaw=1";

}  // namespace

namespace nexus {

NexusViewContainer::NexusViewContainer(Profile* profile)
    : views::WebView(profile) {
  set_allow_accelerators(true);
  if (content::WebContents* web_contents = GetWebContents()) {
    content::WebContentsObserver::Observe(web_contents);
    if (auto* glic_service =
            glic::GlicKeyedServiceFactory::GetGlicKeyedService(profile)) {
      host_ =
          glic_service->host_manager().GetOrCreateHostForStandaloneWebUi(
              web_contents);
      MaybeAttachHost();
    }
  }
  LoadInitialURL(GURL(kNexusWebUiUrl));
}

NexusViewContainer::~NexusViewContainer() = default;

void NexusViewContainer::DidFinishNavigation(
    content::NavigationHandle* navigation_handle) {
  if (!navigation_handle->IsInPrimaryMainFrame() ||
      !navigation_handle->HasCommitted()) {
    return;
  }
  MaybeAttachHost();
}

void NexusViewContainer::MaybeAttachHost() {
  if (!host_) {
    return;
  }
  if (auto* glic_ui = glic::GlicUI::From(GetWebContents())) {
    glic_ui->AttachToHost(host_);
  }
}

}  // namespace nexus
