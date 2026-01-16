#ifndef CUSTOM_BROWSER_UI_VIEW_NAVIGATION_CONTAINER_NATIVE_UI_H_
#define CUSTOM_BROWSER_UI_VIEW_NAVIGATION_CONTAINER_NATIVE_UI_H_

#include "custom_browser/ui/view/native_ui/native_ui.h"

namespace custom_browser::native_ui {

class NavigationContainerNativeUI : public NativeUI {
 public:
  NavigationContainerNativeUI(
      views::View* host_view,
      const custom_browser::mojom::NativeViewParams* layout_params);
  ~NavigationContainerNativeUI() override;

 protected:
  void OnPrepare() override;
  void OnLayout() override;

 private:
  raw_ptr<views::View> back_button_ = nullptr;
  raw_ptr<views::View> forward_button_ = nullptr;
  raw_ptr<views::View> reload_button_ = nullptr;
  raw_ptr<views::View> home_button_ = nullptr;
};

}  // namespace custom_browser::native_ui

#endif  // CUSTOM_BROWSER_UI_VIEW_NAVIGATION_CONTAINER_NATIVE_UI_H_
