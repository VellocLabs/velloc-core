#ifndef CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_TAB_SEARCH_CONTAINER_NATIVE_UI_H_
#define CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_TAB_SEARCH_CONTAINER_NATIVE_UI_H_

#include "base/memory/raw_ptr.h"
#include "chrome/browser/ui/views/tabs/tab_search_container.h"
#include "custom_browser/ui/mojom/browser_constants.mojom-forward.h"
#include "custom_browser/ui/view/native_ui/native_ui.h"
#include "custom_browser/ui/view/native_ui/native_ui_registration_macros.h"

class TabStripRegionView;

namespace custom_browser::native_ui {

// Native UI wrapper for TabSearchContainer that handles the special case
// where TabStripRegionView manually controls the container's position.
// This class nullifies the TabStripRegionView's reference to prevent
// conflicting layout updates when the container is reparented.
class TabSearchContainerNativeUI : public ViewNativeUI {
 public:
  TabSearchContainerNativeUI(
      views::View* host_view,
      const custom_browser::mojom::NativeViewParams* params,
      views::View* view);
  TabSearchContainerNativeUI(const TabSearchContainerNativeUI&) = delete;
  TabSearchContainerNativeUI& operator=(const TabSearchContainerNativeUI&) =
      delete;
  ~TabSearchContainerNativeUI() override;

  DECLARE_NATIVE_UI_CLASS_NAME_FROM_VIEW(TabSearchContainerNativeUI,
                                         TabSearchContainer)

 protected:
  void OnPrepare() override;

 private:
  // Finds the TabStripRegionView that owns the TabSearchContainer.
  TabStripRegionView* FindTabStripRegionView() const;

  // Saved reference to restore on destruction.
  raw_ptr<TabStripRegionView> tab_strip_region_view_ = nullptr;
  raw_ptr<TabSearchContainer> saved_tab_search_container_ = nullptr;
};

}  // namespace custom_browser::native_ui

#endif  // CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_TAB_SEARCH_CONTAINER_NATIVE_UI_H_
