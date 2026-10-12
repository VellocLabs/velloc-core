#ifndef CUSTOM_BROWSER_UI_VIEW_VIEW_VISIBLE_BOUNDS_H_
#define CUSTOM_BROWSER_UI_VIEW_VIEW_VISIBLE_BOUNDS_H_

#include "ui/views/view.h"

namespace custom_browser {

// True when some part of `view` is laid out on screen in its widget
// (`GetVisibleBounds()` non-empty): false when it or an ancestor is hidden,
// unsized or clipped away, or it has no Widget. Gates the actor handoff button
// so it does not float over the chat UI for a selected tab that is not shown.
inline bool HasVisibleBounds(const views::View* view) {
  return view && !view->GetVisibleBounds().IsEmpty();
}

}  // namespace custom_browser

#endif  // CUSTOM_BROWSER_UI_VIEW_VIEW_VISIBLE_BOUNDS_H_
