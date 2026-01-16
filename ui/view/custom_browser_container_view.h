#ifndef CUSTOM_BROWSER_UI_CUSTOM_BROWSER_CONTAINER_VIEW_H_
#define CUSTOM_BROWSER_UI_CUSTOM_BROWSER_CONTAINER_VIEW_H_

#include "base/memory/raw_ptr.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/view.h"
#include "ui/views/view_targeter_delegate.h"

// BrowserView layout (LTR one is pictured here).
//
// --------------------------------------------------------------------
// | BrowserContainerView (top_container_)                                |
// |  --------------------------------------------------------------  |
// |  | Web App toolbar and title (web_app_frame_toolbar_)         |  |
// |  |------------------------------------------------------------|  |
// |  | Tabs (tabstrip_)                                           |  |
// |  |------------------------------------------------------------|  |
// |  | Navigation buttons, address bar, menu (toolbar_)           |  |
// |  --------------------------------------------------------------  |
// |------------------------------------------------------------------|
// | Bookmarks (bookmark_bar_view_)                                   |
// |------------------------------------------------------------------|
// | All infobars (infobar_container_)                                |
// |------------------------------------------------------------------|
// | Contents container (contents_container_)                         |
// |  --------------------------------------------------------------  |
// |  |  devtools_web_view_                                        |  |
// |  |------------------------------------------------------------|  |
// |  |  contents_web_view_ (or multi_contents_view_ if defined)   |  |
// |  --------------------------------------------------------------  |
// --------------------------------------------------------------------

// *******************************************************************

// BrowserView Custom layout (LTR one is pictured here).
//
// --------------------------------------------------------------------
// | CustomBrowserContainerView (browser_container_)                  |
// |  --------------------------------------------------------------  |
// |  | Web App toolbar and title (web_app_frame_toolbar_)         |  |
// |  |------------------------------------------------------------|  |
// |  | ClientWebUIContainerView                                   |  |
// |  |   ---------------------------------------------------------|  |
// |  |   |  Contents container (contents_container_)              |  |
// |  |   |  ------------------------------------------------------|  |
// |  |   |  | devtools_web_view_                                  |  |
// |  |   |  |-----------------------------------------------------|  |
// |  |   |  | contents_web_view_                                  |  |
// |  |   ---------------------------------------------------------|  |
// |  --------------------------------------------------------------  |
// |------------------------------------------------------------------|

class BrowserView;

// Container for the BrowserView's tab strip, toolbar, and sometimes bookmark
// bar. In Chrome OS immersive fullscreen it stacks on top of other views in
// order to slide in and out over the web contents.
class CustomBrowserContainerView : public views::View,
                                   public views::ViewTargeterDelegate {
  METADATA_HEADER(CustomBrowserContainerView, views::View)

 public:
  explicit CustomBrowserContainerView(BrowserView* browser_view);
  CustomBrowserContainerView(const CustomBrowserContainerView&) = delete;
  CustomBrowserContainerView& operator=(const CustomBrowserContainerView&) =
      delete;
  ~CustomBrowserContainerView() override;

  void OnImmersiveRevealUpdated();

  void set_web_app_frame_toolbar(views::View* web_app_frame_toolbar) {
    web_app_frame_toolbar_ = web_app_frame_toolbar;
  }

  // views::View overrides:
  void PaintChildren(const views::PaintInfo& paint_info) override;
  void ChildPreferredSizeChanged(views::View* child) override;

 private:
  // ViewTargeterDelegate:
  bool DoesIntersectRect(const views::View* target,
                         const gfx::Rect& rect) const override;

  // The parent of this view. Not owned.
  raw_ptr<BrowserView> browser_view_;
  raw_ptr<views::View> web_app_frame_toolbar_ = nullptr;
};

#endif  // CUSTOM_BROWSER_UI_CUSTOM_BROWSER_CONTAINER_VIEW_H_
