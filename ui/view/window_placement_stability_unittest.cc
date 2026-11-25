#include "custom_browser/ui/view/window_placement_stability.h"

#include "testing/gtest/include/gtest/gtest.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rect_conversions.h"
#include "ui/gfx/geometry/size.h"

namespace custom_browser {
namespace {

// 150% display scale with 120% system text scale.
constexpr float kFractionalScale = 1.8f;

// The DIP bounds the platform reports for a window of `pixel_bounds`: the
// origin rounded, the size of the enclosing DIP rect.
gfx::Rect ReportedDipBounds(const gfx::Rect& pixel_bounds, float scale) {
  const gfx::Rect enclosing =
      gfx::ScaleToEnclosingRect(pixel_bounds, 1.0f / scale);
  return gfx::Rect(gfx::ScaleToRoundedPoint(pixel_bounds.origin(), 1.0f / scale),
                   enclosing.size());
}

// One close and reopen: the window is saved, then restored from what was saved.
gfx::Size SaveThenRestore(const gfx::Rect& pixel_bounds, float scale, bool fix) {
  gfx::Rect saved = ReportedDipBounds(pixel_bounds, scale);
  if (fix) {
    saved = StableWindowPlacementBounds(saved, pixel_bounds.size(), scale);
  }
  return RestoredPixelSize(saved, scale);
}

TEST(WindowPlacementStabilityTest, ReportedBoundsGrowTheWindowOnEveryReopen) {
  // The reported bug, without the correction: a cycle adds pixels on an axis
  // whose size is not a whole number of DIP, and never removes any.
  gfx::Rect window(36, 36, 3813, 2052);
  const gfx::Size first = SaveThenRestore(window, kFractionalScale, false);
  EXPECT_GT(first.width(), window.width());
  EXPECT_GE(first.height(), window.height());
}

TEST(WindowPlacementStabilityTest, CorrectedBoundsNeverGrowTheWindow) {
  gfx::Rect window(36, 36, 3813, 2052);
  for (int cycle = 0; cycle < 20; ++cycle) {
    const gfx::Size restored = SaveThenRestore(window, kFractionalScale, true);
    EXPECT_LE(restored.width(), window.width()) << "cycle " << cycle;
    EXPECT_LE(restored.height(), window.height()) << "cycle " << cycle;
    window.set_size(restored);
  }
}

TEST(WindowPlacementStabilityTest, CorrectedBoundsSettleAfterOneReopen) {
  const gfx::Rect window(36, 36, 3813, 2052);
  const gfx::Size once = SaveThenRestore(window, kFractionalScale, true);
  const gfx::Size twice = SaveThenRestore(gfx::Rect(window.origin(), once),
                                          kFractionalScale, true);
  EXPECT_EQ(once, twice);
  // The one-time loss is under one DIP per axis.
  EXPECT_GT(once.width(), window.width() - 2);
  EXPECT_GT(once.height(), window.height() - 2);
}

TEST(WindowPlacementStabilityTest, SettlesForManySizesAndScales) {
  for (float scale : {1.25f, 1.5f, 1.75f, 1.8f, 2.25f, 2.7f}) {
    for (int width = 900; width < 960; ++width) {
      const gfx::Rect window(37, 41, width, width - 200);
      const gfx::Size once = SaveThenRestore(window, scale, true);
      EXPECT_LE(once.width(), window.width()) << scale << " " << width;
      EXPECT_LE(once.height(), window.height()) << scale << " " << width;
      const gfx::Size twice =
          SaveThenRestore(gfx::Rect(window.origin(), once), scale, true);
      EXPECT_EQ(once, twice) << scale << " " << width;
    }
  }
}

TEST(WindowPlacementStabilityTest, KeepsTheOrigin) {
  const gfx::Rect reported(20, 20, 2120, 1141);
  const gfx::Rect stable = StableWindowPlacementBounds(
      reported, gfx::Size(3813, 2052), kFractionalScale);
  EXPECT_EQ(reported.origin(), stable.origin());
  EXPECT_LT(stable.width(), reported.width());
  EXPECT_LT(stable.height(), reported.height());
}

TEST(WindowPlacementStabilityTest, IntegerScaleIsLeftAlone) {
  const gfx::Rect reported(20, 20, 1200, 800);
  EXPECT_EQ(reported, StableWindowPlacementBounds(
                          reported, gfx::Size(2400, 1600), 2.0f));
}

TEST(WindowPlacementStabilityTest, UnusableInputIsLeftAlone) {
  const gfx::Rect reported(20, 20, 1200, 800);
  EXPECT_EQ(reported,
            StableWindowPlacementBounds(reported, gfx::Size(), 1.8f));
  EXPECT_EQ(reported, StableWindowPlacementBounds(
                          reported, gfx::Size(2160, 1440), 0.f));
  EXPECT_EQ(gfx::Rect(), StableWindowPlacementBounds(
                             gfx::Rect(), gfx::Size(2160, 1440), 1.8f));
  // A pixel size far from the reported bounds is not a rounding difference.
  EXPECT_EQ(reported, StableWindowPlacementBounds(
                          reported, gfx::Size(400, 300), 1.8f));
}

}  // namespace
}  // namespace custom_browser
