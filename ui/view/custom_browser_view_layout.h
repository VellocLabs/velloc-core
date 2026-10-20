#ifndef CUSTOM_BROWSER_UI_CUSTOM_BROWSER_VIEW_LAYOUT_H_
#define CUSTOM_BROWSER_UI_CUSTOM_BROWSER_VIEW_LAYOUT_H_

#include "base/memory/raw_ptr.h"
#include "chrome/browser/ui/views/frame/layout/browser_view_layout.h"
#include "chrome/browser/ui/views/frame/layout/browser_view_layout_impl.h"
#include "chrome/browser/ui/views/frame/layout/browser_view_layout_params.h"
#include "chrome/browser/ui/views/frame/layout/browser_view_tabbed_layout_impl.h"
#include "custom_browser/ui/view/undisplayed_contents_sizer.h"
#include "ui/gfx/geometry/size.h"
#include "ui/views/layout/flex_layout_types.h"


class BrowserWindowInterface;
class CustomBrowserWebUIView;
class CustomBrowserView;
class NativeUIContainerView;
struct CustomBrowserViews;

namespace custom_browser {
class CustomBrowserViewLayout : public BrowserViewTabbedLayoutImpl {
 public:
  CustomBrowserViewLayout(std::unique_ptr<BrowserViewLayoutDelegate> delegate,
                          BrowserWindowInterface* browser,
                          BrowserViewLayoutViews views);
  ~CustomBrowserViewLayout() override;

  void LayoutContentsContainerView(gfx::Rect& contents_container_bounds,
                                   bool visible);
  // Gives every tab of this window that no view has sized yet a real size:
  // the size the contents area last had while shown, else the window's client
  // area. A tab activated while <main-contents-view> is hidden is attached to
  // a hidden NativeViewHost, which never sizes it, so it would otherwise stay
  // 0x0 and break observation, screenshots and input
  // (undisplayed_contents_sizer.h). Runs after every contents layout and on
  // every active-tab change (CustomBrowserView::OnActiveTabChanged).
  void SizeUndisplayedTabs();

 protected:
  CustomBrowserView* GetCustomBrowserView() const;
  NativeUIContainerView* GetNativeUIContainerView() const;
  CustomBrowserViews& custom_views() const;
  void FixViewsParentForCustom();
  void SetViewBoundAndVisibility(
      views::View* child,
      const gfx::Rect& bounds_,
      std::optional<bool> visibility_ = std::nullopt) const;
  ProposedLayout CalculateProposedLayout(
      const BrowserLayoutParams& params) const override;

 private:
  // The window this layout belongs to; null in unit tests.
  BrowserWindowInterface* browser() const { return browser_; }

  const raw_ptr<BrowserWindowInterface> browser_;
  UndisplayedContentsSizer undisplayed_contents_sizer_;
};

}  // namespace custom_browser

#endif  // CUSTOM_BROWSER_UI_CUSTOM_BROWSER_VIEW_LAYOUT_H_
