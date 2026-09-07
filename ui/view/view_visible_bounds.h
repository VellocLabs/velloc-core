#ifndef CUSTOM_BROWSER_UI_VIEW_VIEW_VISIBLE_BOUNDS_H_
#define CUSTOM_BROWSER_UI_VIEW_VIEW_VISIBLE_BOUNDS_H_

#include "ui/views/view.h"

namespace custom_browser {

// True when some part of `view` is laid out on screen in its widget, i.e.
// `views::View::GetVisibleBounds()` is non-empty. That is empty when `view`
// or any ancestor is hidden, when `view` or any ancestor has no size, when
// the ancestors clip it away entirely, and for a view that is not in a
// Widget. It is the same test `views::NativeViewHost::Layout` uses to decide
// whether to show the native view it hosts, so for a contents WebView it
// answers "is the tab on screen?".
//
// Why it exists: in the kNexus window the native contents container (the
// area that shows the agent's browser tab) is a guest-positioned overlay.
// Nothing of it is on screen in two states:
//  * `CustomBrowserViewLayout::LayoutContentsContainerView` hid it
//    (`SetVisible(false)`), because the active chat session owns no tab here.
//  * It was never laid out: Chromium creates it visible with 0x0 bounds, and
//    the layout only runs once `<main-contents-view>` has reported itself
//    visible. `CustomBrowserUiPrefs::UpdateLayouts` drops a hidden report for
//    a view it does not track yet, and the kNexus holder reports hidden until
//    a guest shows a tab. On a profile that never showed one, that is every
//    report a scoped runtime connection (a remote client, a manager) or a
//    background session produces.
// A background or scoped session's actor task still has its tab *selected*
// in the strip, so upstream's actor UI, which keys the handoff ("Take over
// task" / pause) button on `TabInterface::IsSelected()` alone, would float
// the button over the chat UI. The hook in
// `HandoffButtonController::UpdateState` ANDs this predicate on the button's
// anchor (the contents WebView). A view-chain visibility check covered only
// the first state; the second is visible all the way up, and 0x0.
inline bool HasVisibleBounds(const views::View* view) {
  return view && !view->GetVisibleBounds().IsEmpty();
}

}  // namespace custom_browser

#endif  // CUSTOM_BROWSER_UI_VIEW_VIEW_VISIBLE_BOUNDS_H_
