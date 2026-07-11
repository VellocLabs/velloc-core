#ifndef CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_TAB_STRIP_ACTION_CONTAINER_NATIVE_UI_H_
#define CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_TAB_STRIP_ACTION_CONTAINER_NATIVE_UI_H_

#include "base/memory/raw_ptr.h"
#include "chrome/browser/ui/views/tabs/tab_strip_action_container.h"
#include "custom_browser/ui/mojom/browser_constants.mojom-forward.h"
#include "custom_browser/ui/view/native_ui/native_ui.h"
#include "custom_browser/ui/view/native_ui/native_ui_registration_macros.h"
#include "custom_browser/ui/view/native_ui/scoped_view_state.h"

namespace custom_browser::native_ui {

// Native UI wrapper for TabStripActionContainer that handles the special case
// where TabStripRegionView manually controls the container's position.
// This class nullifies the TabStripRegionView's reference to prevent
// conflicting layout updates when the container is reparented.
class TabStripActionContainerNativeUI : public ViewNativeUI {
 public:
  TabStripActionContainerNativeUI(
      views::View* host_view,
      const custom_browser::mojom::NativeViewParams* params,
      views::View* view);
  TabStripActionContainerNativeUI(const TabStripActionContainerNativeUI&) =
      delete;
  TabStripActionContainerNativeUI& operator=(
      const TabStripActionContainerNativeUI&) = delete;
  ~TabStripActionContainerNativeUI() override;

  DECLARE_NATIVE_UI_CLASS_NAME_FROM_VIEW(TabStripActionContainerNativeUI,
                                         TabStripActionContainer)

 protected:
  void OnPrepare() override;

 private:
  // Tracks temporary tab strip action container state and restores on teardown.
  ScopedViewState<TabStripActionContainer> tab_strip_action_container_state_;
};

}  // namespace custom_browser::native_ui

#endif  // CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_TAB_STRIP_ACTION_CONTAINER_NATIVE_UI_H_
