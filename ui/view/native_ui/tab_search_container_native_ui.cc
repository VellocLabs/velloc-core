#include "custom_browser/ui/view/native_ui/tab_search_container_native_ui.h"

#include "chrome/browser/ui/views/frame/tab_strip_region_view.h"
#include "chrome/browser/ui/views/tabs/tab_search_container.h"
#include "custom_browser/ui/view/custom_browser_view.h"
#include "ui/views/view.h"

namespace custom_browser::native_ui {

TabSearchContainerNativeUI::TabSearchContainerNativeUI(
    views::View* host_view,
    const custom_browser::mojom::NativeViewParams* params,
    views::View* view)
    : ViewNativeUI(host_view, params, view) {}

TabSearchContainerNativeUI::~TabSearchContainerNativeUI() {
  // Restore the TabStripRegionView's reference to TabSearchContainer.
  if (tab_strip_region_view_ && saved_tab_search_container_) {
    tab_strip_region_view_->tab_search_container_ = saved_tab_search_container_;
    tab_strip_region_view_->AddChildView(saved_tab_search_container_);
    saved_tab_search_container_->SetVisible(true);
  }
  // Prevent NativeUI::~NativeUI() from hiding the view after restore.
  if (!views_.empty()) {
    views_[0] = nullptr;
  }
}

void TabSearchContainerNativeUI::OnPrepare() {
  // Find the TabStripRegionView and nullify its reference to prevent
  // conflicting layout updates.
  tab_strip_region_view_ = FindTabStripRegionView();
  if (tab_strip_region_view_) {
    // Save the reference so we can restore it in the destructor.
    saved_tab_search_container_ = tab_strip_region_view_->tab_search_container_;
    // Nullify the reference to prevent TabStripRegionView::Layout() from
    // overriding the bounds.
    tab_strip_region_view_->tab_search_container_ = nullptr;
  }

  // Call the base class OnPrepare to reparent the view.
  ViewNativeUI::OnPrepare();
}

TabStripRegionView* TabSearchContainerNativeUI::FindTabStripRegionView() const {
  // The TabSearchContainer's original parent should be the TabStripRegionView.
  views::View* tab_search_container = view();
  if (!tab_search_container) {
    return nullptr;
  }

  views::View* parent = tab_search_container->parent();
  if (!parent) {
    return nullptr;
  }

  // Check if the parent is a TabStripRegionView.
  if (parent->GetClassName() == TabStripRegionView::kViewClassName) {
    return static_cast<TabStripRegionView*>(parent);
  }

  return nullptr;
}

}  // namespace custom_browser::native_ui
