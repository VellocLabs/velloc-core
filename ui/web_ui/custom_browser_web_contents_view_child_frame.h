#ifndef CUSTOM_BROWSER_UI_WEB_UI_CUSTOM_BROWSER_WEB_CONTENTS_VIEW_CHILD_FRAME_H_
#define CUSTOM_BROWSER_UI_WEB_UI_CUSTOM_BROWSER_WEB_CONTENTS_VIEW_CHILD_FRAME_H_

#include "content/browser/web_contents/web_contents_view_child_frame.h"

namespace content {
class WebContentsImpl;
class WebContentsViewDelegate;
class RenderFrameHost;
struct ContextMenuParams;
}  // namespace content

class CustomBrowserWebContentsViewChildFrame
    : public content::WebContentsViewChildFrame {
 public:
  CustomBrowserWebContentsViewChildFrame(
      content::WebContentsImpl* web_contents,
      std::unique_ptr<content::WebContentsViewDelegate> delegate,
      raw_ptr<content::RenderViewHostDelegateView>* delegate_view);
  ~CustomBrowserWebContentsViewChildFrame() override;

  void ShowContextMenu(content::RenderFrameHost& render_frame_host,
                       const content::ContextMenuParams& params) override;
  void Focus() override;
  void SetInitialFocus() override;
  void StoreFocus() override;
  void RestoreFocus() override;
  void FocusThroughTabTraversal(bool reverse) override;
  // Add any additional functionality specific to CustomBrowser here

 private:
  // Add any private members specific to CustomBrowser here
};

#define WebContentsViewChildFrame CustomBrowserWebContentsViewChildFrame
#endif  // CUSTOM_BROWSER_UI_WEB_UI_CUSTOM_BROWSER_WEB_CONTENTS_VIEW_CHILD_FRAME_H_
