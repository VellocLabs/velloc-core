#include "custom_browser/ui/view/window_placement_stability.h"

#include "ui/gfx/geometry/rect_conversions.h"

namespace custom_browser {

namespace {

// Rounding adds less than one DIP per edge; a reported size further off than
// this was not produced by rounding and is left alone.
constexpr int kMaxShrinkDip = 4;

}  // namespace

gfx::Size RestoredPixelSize(const gfx::Rect& dip_bounds, float scale) {
  return gfx::ScaleToEnclosingRect(dip_bounds, scale).size();
}

gfx::Rect StableWindowPlacementBounds(const gfx::Rect& dip_bounds,
                                      const gfx::Size& pixel_size,
                                      float scale) {
  if (dip_bounds.IsEmpty() || pixel_size.IsEmpty() || scale <= 0.f) {
    return dip_bounds;
  }
  gfx::Rect stable = dip_bounds;
  for (int i = 0; i < kMaxShrinkDip && stable.width() > 1 &&
                  RestoredPixelSize(stable, scale).width() > pixel_size.width();
       ++i) {
    stable.set_width(stable.width() - 1);
  }
  for (int i = 0;
       i < kMaxShrinkDip && stable.height() > 1 &&
       RestoredPixelSize(stable, scale).height() > pixel_size.height();
       ++i) {
    stable.set_height(stable.height() - 1);
  }
  const gfx::Size restored = RestoredPixelSize(stable, scale);
  if (restored.width() > pixel_size.width() ||
      restored.height() > pixel_size.height()) {
    return dip_bounds;
  }
  return stable;
}

}  // namespace custom_browser
