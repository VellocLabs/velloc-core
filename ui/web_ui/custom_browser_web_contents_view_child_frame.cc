#include "custom_browser/ui/web_ui/custom_browser_web_contents_view_child_frame.h"

#include <memory>
#include <utility>

#include "base/check.h"
#include "content/browser/renderer_host/render_widget_host_view_child_frame.h"
#include "content/browser/web_contents/web_contents_impl.h"
#include "content/public/browser/web_contents_view_delegate.h"
#undef WebContentsViewChildFrame

CustomBrowserWebContentsViewChildFrame::CustomBrowserWebContentsViewChildFrame(
    content::WebContentsImpl* web_contents,
    std::unique_ptr<content::WebContentsViewDelegate> delegate,
    raw_ptr<content::RenderViewHostDelegateView>* delegate_view)
    : content::WebContentsViewChildFrame(web_contents,
                                         std::move(delegate),
                                         delegate_view) {
  // Additional initialization for CustomBrowser if needed
}

CustomBrowserWebContentsViewChildFrame::~CustomBrowserWebContentsViewChildFrame() = default;

void CustomBrowserWebContentsViewChildFrame::ShowContextMenu(
    content::RenderFrameHost& render_frame_host,
    const content::ContextMenuParams& params) {
   if (delegate_) {
    delegate_->ShowContextMenu(render_frame_host, params);
    // WARNING: we may have been deleted during the call to ShowContextMenu().
  }
}
void CustomBrowserWebContentsViewChildFrame::Focus() {
  if (delegate_)
    delegate_->ResetStoredFocus();

  if (delegate_ && delegate_->Focus())
    return;
  // todo: Implement focus logic for CustomBrowser
  // RenderWidgetHostView* rwhv = web_contents_->GetRenderWidgetHostView();
  // if (rwhv)
  //   rwhv->Focus();
}

void CustomBrowserWebContentsViewChildFrame::SetInitialFocus() {
  // if (delegate_)
  //   delegate_->ResetStoredFocus();

  // if (web_contents_->FocusLocationBarByDefault())
  //   web_contents_->SetFocusToLocationBar();
  // else
  //   Focus();
}

void CustomBrowserWebContentsViewChildFrame::StoreFocus() {
  // if (delegate_)
  //   delegate_->StoreFocus();
}

void CustomBrowserWebContentsViewChildFrame::RestoreFocus() {
  // if (delegate_ && delegate_->RestoreFocus())
  //   return;
  // SetInitialFocus();
}

void CustomBrowserWebContentsViewChildFrame::FocusThroughTabTraversal(bool reverse) {
  // if (delegate_)
  //   delegate_->ResetStoredFocus();

  // web_contents_->GetRenderViewHost()->SetInitialFocus(reverse);
}