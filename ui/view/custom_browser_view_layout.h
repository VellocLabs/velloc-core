#ifndef CUSTOM_BROWSER_UI_CUSTOM_BROWSER_VIEW_LAYOUT_H_
#define CUSTOM_BROWSER_UI_CUSTOM_BROWSER_VIEW_LAYOUT_H_

#include "chrome/browser/ui/views/frame/layout/browser_view_layout.h"
#include "chrome/browser/ui/views/frame/layout/browser_view_layout_impl.h"
#include "chrome/browser/ui/views/frame/layout/browser_view_layout_params.h"
#include "chrome/browser/ui/views/frame/layout/browser_view_tabbed_layout_impl.h"
#include "ui/gfx/geometry/size.h"
#include "ui/views/layout/flex_layout_types.h"


class CustomBrowserWebUIView;
class CustomBrowserView;
class NativeUIContainerView;
struct CustomBrowserViews;

namespace custom_browser {
class CustomBrowserViewLayout : public BrowserViewTabbedLayoutImpl {
 public:
  CustomBrowserViewLayout(std::unique_ptr<BrowserViewLayoutDelegate> delegate,
                          Browser* browser,
                          BrowserViewLayoutViews views);
  ~CustomBrowserViewLayout() override;

  void LayoutContentsContainerView(gfx::Rect& contents_container_bounds,
                                   bool visible);
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
};

}  // namespace custom_browser

#endif  // CUSTOM_BROWSER_UI_CUSTOM_BROWSER_VIEW_LAYOUT_H_
