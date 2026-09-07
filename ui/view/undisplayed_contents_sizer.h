#ifndef CUSTOM_BROWSER_UI_VIEW_UNDISPLAYED_CONTENTS_SIZER_H_
#define CUSTOM_BROWSER_UI_VIEW_UNDISPLAYED_CONTENTS_SIZER_H_

#include <optional>

#include "ui/gfx/geometry/size.h"

namespace custom_browser {

// Decides the size of a tab's WebContents that no view has ever sized.
//
// Why it exists: in the kNexus window the native contents container (the area
// that shows the agent's browser tab) is a guest-positioned overlay, and
// `CustomBrowserViewLayout::LayoutContentsContainerView` hides it whenever no
// guest shows the tab. That happens when the user is looking at another chat
// session, or when the session is driven only by a scoped runtime connection
// (a remote client, a manager) that cannot publish `shell.publishLayout`. A
// hidden views::NativeViewHost never sizes the native view it hosts
// (NativeViewHost::Layout only calls HideWidget), so a tab that becomes active
// while the container is hidden keeps the 0x0 its WebContents was created
// with. The page then reports innerWidth/innerHeight 0, observation and
// screenshots fail, and actor input lands offscreen. A tab that WAS shown
// keeps its last size after it is hidden, which is why only never-shown tabs
// break.
//
// Rule: a non-empty size is never touched. The tab keeps what the layout last
// gave it, and the NativeViewHost resizes it as soon as it is shown. An empty
// size takes the size the contents area last had while shown, or, before
// anything was ever shown, `fallback_size` (the window's client area). If
// every candidate is empty (a window not laid out yet), nothing is decided and
// a later layout pass decides.
//
// Header-only and //ui/gfx-only so the rule is unit-testable without a
// BrowserView (see undisplayed_contents_sizer_unittest.cc).
class UndisplayedContentsSizer {
 public:
  // Feed every contents-area layout. Only a shown, non-empty area is
  // remembered. A hidden <main-contents-view> reports a 0x0 rect, and that
  // report must not replace the last real size.
  void OnContentsAreaLaidOut(const gfx::Size& size, bool visible) {
    if (visible && !size.IsEmpty()) {
      last_shown_size_ = size;
    }
  }

  // The size to give a tab whose WebContents is `contents_size` now, or
  // nullopt to leave it alone.
  std::optional<gfx::Size> SizeFor(const gfx::Size& contents_size,
                                   const gfx::Size& fallback_size) const {
    if (!contents_size.IsEmpty()) {
      return std::nullopt;
    }
    if (!last_shown_size_.IsEmpty()) {
      return last_shown_size_;
    }
    if (!fallback_size.IsEmpty()) {
      return fallback_size;
    }
    return std::nullopt;
  }

  const gfx::Size& last_shown_size() const { return last_shown_size_; }

 private:
  gfx::Size last_shown_size_;
};

}  // namespace custom_browser

#endif  // CUSTOM_BROWSER_UI_VIEW_UNDISPLAYED_CONTENTS_SIZER_H_
