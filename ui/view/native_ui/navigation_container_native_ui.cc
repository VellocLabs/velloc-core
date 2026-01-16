#include "custom_browser/ui/view/native_ui/navigation_container_native_ui.h"

#include "chrome/browser/ui/views/toolbar/toolbar_view.h"
#include "custom_browser/ui/view/custom_browser_view.h"
#include "ui/views/view.h"

namespace custom_browser::native_ui {

NavigationContainerNativeUI::NavigationContainerNativeUI(
    views::View* host_view,
    const custom_browser::mojom::NativeViewParams* layout_params)
    : NativeUI(host_view, layout_params) {
  DCHECK(host_view);
  // back_button_ = host_view_->toolbar()->GetBackButton();
  // forward_button_ = host_view_->toolbar()->GetForwardButton();
  // reload_button_ = host_view_->toolbar()->GetReloadButton();
  // home_button_ = host_view_->toolbar()->GetHomeButton();
  DCHECK(back_button_ && forward_button_ && reload_button_ && home_button_);
}

NavigationContainerNativeUI::~NavigationContainerNativeUI() = default;

void NavigationContainerNativeUI::OnPrepare() {
  NativeUI::OnPrepare();
}

void NavigationContainerNativeUI::OnLayout() {
  // const bool should_show = visible && !force_hidden;
  // if (!navigation_view_)
  //   navigation_view_ = host_view_ ? host_view_->top_container() : nullptr;
  // if (!navigation_view_)
  //   return;

  // navigation_view_->SetVisible(should_show);
  // if (should_show)
  //   navigation_view_->SetBoundsRect(bounds_);
}

}  // namespace custom_browser::native_ui
