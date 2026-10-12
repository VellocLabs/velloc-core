#ifndef CUSTOM_BROWSER_UI_VIEW_UNDISPLAYED_CONTENTS_SIZER_H_
#define CUSTOM_BROWSER_UI_VIEW_UNDISPLAYED_CONTENTS_SIZER_H_

#include <optional>

#include "ui/gfx/geometry/size.h"

namespace custom_browser {

// Decides the size of a tab's WebContents that no view has ever sized (a
// hidden NativeViewHost never sizes its view, so it would stay 0x0). A
// non-empty size is never touched; an empty one takes the contents area's last
// shown size, else `fallback_size`; if every candidate is empty, none decides.
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
