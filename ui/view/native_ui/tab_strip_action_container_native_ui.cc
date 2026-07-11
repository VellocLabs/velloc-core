#include "custom_browser/ui/view/native_ui/tab_strip_action_container_native_ui.h"

#include "chrome/browser/ui/views/frame/horizontal_tab_strip_region_view.h"
#include "chrome/browser/ui/views/frame/tab_strip_region_view.h"
#include "chrome/browser/ui/views/tabs/tab_strip_action_container.h"
#include "custom_browser/ui/view/custom_browser_view.h"
#include "custom_browser/ui/view/native_ui/native_ui_utils.h"
#include "ui/views/view.h"

namespace custom_browser::native_ui {

TabStripActionContainerNativeUI::TabStripActionContainerNativeUI(
    views::View* host_view,
    const custom_browser::mojom::NativeViewParams* params,
    views::View* view)
    : ViewNativeUI(host_view, params, view) {}

TabStripActionContainerNativeUI::~TabStripActionContainerNativeUI() {
  TabStripActionContainer* tab_strip_action_container =
      tab_strip_action_container_state_.view();
  tab_strip_action_container_state_.Reset();
  if (tab_strip_action_container) {
    tab_strip_action_container->SetVisible(true);
  }

  // Prevent NativeUI::~NativeUI() from hiding the view after restore.
  if (!views_.empty()) {
    views_[0] = nullptr;
  }
}

void TabStripActionContainerNativeUI::OnPrepare() {
  // Find the TabStripRegionView and clear its reference while this native UI
  // controls the TabStripActionContainer placement.
  auto* tab_strip_region_view =
      utils::FindTabStripRegionView(GetCustomBrowserView());
  if (!tab_strip_region_view) {
    return;
  }
  auto* horizontal_view =
      static_cast<HorizontalTabStripRegionView*>(tab_strip_region_view);

  tab_strip_action_container_state_ =
      horizontal_view->tab_strip_action_container_;
  if (tab_strip_action_container_state_) {
    tab_strip_action_container_state_.TemporarilyClearReference(
        &horizontal_view->tab_strip_action_container_);
    tab_strip_action_container_state_.SetParent(host_view());
    tab_strip_action_container_state_.EnsureLayer();
    tab_strip_action_container_state_.SetFillsBoundsOpaquely(false);
  }
}

}  // namespace custom_browser::native_ui
