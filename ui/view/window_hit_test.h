#ifndef CUSTOM_BROWSER_UI_VIEW_WINDOW_HIT_TEST_H_
#define CUSTOM_BROWSER_UI_VIEW_WINDOW_HIT_TEST_H_

namespace custom_browser {

// A resize gutter along the inside of the client view's edges.
//
// On Windows the OS resize border lives *outside* the client area (WM_NCCALCSIZE
// insets the client rect by the frame thickness on the left, right and bottom),
// and it is mostly in the invisible margin outside the visible window edge. The
// pixels just *inside* the visible edge therefore belong to the client view,
// which answers HTCLIENT for them. Sweeping the pointer along the edge crosses
// that boundary repeatedly, so the cursor flips between the size cursor
// (WM_SETCURSOR sees HTRIGHT from DefWindowProc) and the arrow (WM_SETCURSOR
// sees HTCLIENT) — the reported "size cursor flashes once, then reverts", while
// a click that lands on an outside pixel still starts a resize.
//
// RootView deliberately does not fight over this: it skips its own
// widget_->SetCursor() when the move carries ui::EF_IS_NON_CLIENT (see
// RootView::HandleMouseEnteredOrMoved). So the whole cursor decision is the
// WM_NCHITTEST answer, and making the client view claim a few pixels along its
// own edges is what makes that answer — and the cursor — stable.
//
// Kept dependency free (plain ints, no gfx) so it is unit-testable without
// standing up a BrowserView.
enum class ResizeEdge {
  kNone,
  kLeft,
  kRight,
  kTop,
  kBottom,
  kTopLeft,
  kTopRight,
  kBottomLeft,
  kBottomRight,
};

// Width of the gutter, in DIP. Deliberately narrow: these pixels stop reaching
// the page, and the window edge is where a vertical scrollbar sits.
inline constexpr int kResizeGutterThickness = 4;

// Within this distance of a corner, the edges become diagonal resize handles.
// Matches the kResizeCornerWidth used by BrowserFrameViewWin.
inline constexpr int kResizeGutterCornerSize = 16;

// Which resize edge (`x`, `y`) falls in for a view of `width` x `height`.
// kNone when the point is outside the gutter.
constexpr ResizeEdge ResizeEdgeForPoint(int x,
                                        int y,
                                        int width,
                                        int height,
                                        int thickness = kResizeGutterThickness,
                                        int corner = kResizeGutterCornerSize) {
  if (x < 0 || y < 0 || x >= width || y >= height) {
    return ResizeEdge::kNone;
  }
  const bool in_left = x < thickness;
  const bool in_right = x >= width - thickness;
  const bool in_top = y < thickness;
  const bool in_bottom = y >= height - thickness;
  if (!in_left && !in_right && !in_top && !in_bottom) {
    return ResizeEdge::kNone;
  }

  // Near a corner, an edge hit becomes the diagonal.
  const bool near_left = x < corner;
  const bool near_right = x >= width - corner;
  const bool near_top = y < corner;
  const bool near_bottom = y >= height - corner;

  if (in_top) {
    if (near_left) {
      return ResizeEdge::kTopLeft;
    }
    if (near_right) {
      return ResizeEdge::kTopRight;
    }
    return ResizeEdge::kTop;
  }
  if (in_bottom) {
    if (near_left) {
      return ResizeEdge::kBottomLeft;
    }
    if (near_right) {
      return ResizeEdge::kBottomRight;
    }
    return ResizeEdge::kBottom;
  }
  if (in_left) {
    return near_top ? ResizeEdge::kTopLeft
                    : (near_bottom ? ResizeEdge::kBottomLeft
                                   : ResizeEdge::kLeft);
  }
  return near_top
             ? ResizeEdge::kTopRight
             : (near_bottom ? ResizeEdge::kBottomRight : ResizeEdge::kRight);
}

}  // namespace custom_browser

#endif  // CUSTOM_BROWSER_UI_VIEW_WINDOW_HIT_TEST_H_
