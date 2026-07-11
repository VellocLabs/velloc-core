#ifndef CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_NEXUS_VIEW_CONTAINER_NATIVE_UI_H_
#define CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_NEXUS_VIEW_CONTAINER_NATIVE_UI_H_

#include <string_view>

#include "custom_browser/ui/view/native_ui/native_ui.h"
#include "custom_browser/ui/view/native_ui/native_ui_registration_macros.h"

namespace custom_browser::native_ui {

class NexusViewContainerNativeUI : public NativeUI {
 public:
  NexusViewContainerNativeUI(
      views::View* host_view,
      const custom_browser::mojom::NativeViewParams* layout_params);
  ~NexusViewContainerNativeUI() override;

  DECLARE_NATIVE_UI_CLASS_NAME(NexusViewContainerNativeUI,
                               "nexus-view-container")

 protected:
  void OnPrepare() override;
  void OnLayout() override;
};

}  // namespace custom_browser::native_ui

#endif  // CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_NEXUS_VIEW_CONTAINER_NATIVE_UI_H_
