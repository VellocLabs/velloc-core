#include "custom_browser/ui/view/window_hit_test.h"

#include "testing/gtest/include/gtest/gtest.h"

namespace custom_browser {
namespace {

// A view big enough that the 16-DIP corner zones don't meet in the middle.
constexpr int kW = 200;
constexpr int kH = 100;
constexpr int kT = 4;   // gutter thickness
constexpr int kC = 16;  // corner size

ResizeEdge At(int x, int y) {
  return ResizeEdgeForPoint(x, y, kW, kH, kT, kC);
}

TEST(WindowHitTestTest, InteriorIsNotAResizeEdge) {
  EXPECT_EQ(ResizeEdge::kNone, At(kW / 2, kH / 2));
  // Just inside the gutter on every side.
  EXPECT_EQ(ResizeEdge::kNone, At(kT, kH / 2));
  EXPECT_EQ(ResizeEdge::kNone, At(kW - kT - 1, kH / 2));
  EXPECT_EQ(ResizeEdge::kNone, At(kW / 2, kT));
  EXPECT_EQ(ResizeEdge::kNone, At(kW / 2, kH - kT - 1));
}

// The reported bug: the visible right and bottom edges must answer a resize
// component rather than falling through to HTCLIENT, so WM_SETCURSOR shows the
// size cursor consistently instead of flipping to the arrow.
TEST(WindowHitTestTest, TrailingEdgesAreResizeEdges) {
  EXPECT_EQ(ResizeEdge::kRight, At(kW - 1, kH / 2));
  EXPECT_EQ(ResizeEdge::kBottom, At(kW / 2, kH - 1));
}

TEST(WindowHitTestTest, LeadingEdgesAreResizeEdges) {
  EXPECT_EQ(ResizeEdge::kLeft, At(0, kH / 2));
  EXPECT_EQ(ResizeEdge::kTop, At(kW / 2, 0));
}

TEST(WindowHitTestTest, CornersResolveToDiagonals) {
  EXPECT_EQ(ResizeEdge::kTopLeft, At(0, 0));
  EXPECT_EQ(ResizeEdge::kTopRight, At(kW - 1, 0));
  EXPECT_EQ(ResizeEdge::kBottomLeft, At(0, kH - 1));
  EXPECT_EQ(ResizeEdge::kBottomRight, At(kW - 1, kH - 1));
}

// An edge hit within `corner` of a corner becomes the diagonal, so the whole
// corner region resizes both axes rather than only one.
TEST(WindowHitTestTest, EdgeNearCornerBecomesDiagonal) {
  EXPECT_EQ(ResizeEdge::kTopRight, At(kW - kC, 0));
  EXPECT_EQ(ResizeEdge::kBottomRight, At(kW - 1, kH - kC));
  // Beyond the corner zone it is a plain edge again.
  EXPECT_EQ(ResizeEdge::kTop, At(kW - kC - 1, 0));
  EXPECT_EQ(ResizeEdge::kRight, At(kW - 1, kH - kC - 1));
}

// The gutter is measured from the view's own bounds, so out-of-range points
// (the OS frame outside the client view) are never claimed here — they belong
// to DefWindowProc.
TEST(WindowHitTestTest, PointsOutsideTheViewAreNotClaimed) {
  EXPECT_EQ(ResizeEdge::kNone, At(-1, kH / 2));
  EXPECT_EQ(ResizeEdge::kNone, At(kW, kH / 2));
  EXPECT_EQ(ResizeEdge::kNone, At(kW / 2, -1));
  EXPECT_EQ(ResizeEdge::kNone, At(kW / 2, kH));
}

}  // namespace
}  // namespace custom_browser
