#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_VIEW_NEXUS_VIEW_CONTAINER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_VIEW_NEXUS_VIEW_CONTAINER_H_

#include "base/memory/raw_ptr.h"
#include "ui/views/controls/webview/webview.h"

class Profile;
namespace content {
class NavigationHandle;
}
namespace glic {
class Host;
}

namespace nexus {

class NexusViewContainer : public views::WebView {
 public:
  explicit NexusViewContainer(Profile* profile);

  NexusViewContainer(const NexusViewContainer&) = delete;
  NexusViewContainer& operator=(const NexusViewContainer&) = delete;

  ~NexusViewContainer() override;

 private:
  void DidFinishNavigation(
      content::NavigationHandle* navigation_handle) override;
  void MaybeAttachHost();

  raw_ptr<glic::Host> host_ = nullptr;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_VIEW_NEXUS_VIEW_CONTAINER_H_
