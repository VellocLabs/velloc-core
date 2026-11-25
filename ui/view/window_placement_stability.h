#ifndef CUSTOM_BROWSER_UI_VIEW_WINDOW_PLACEMENT_STABILITY_H_
#define CUSTOM_BROWSER_UI_VIEW_WINDOW_PLACEMENT_STABILITY_H_

#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"

namespace custom_browser {

// Contract: returns the window bounds, in DIP, to save for a window whose
// restored size is `pixel_size` on a display of `scale`, given the DIP bounds
// `dip_bounds` the platform reported for it. The origin is kept. The size is
// the largest one, no larger than the reported one, that restores to at most
// `pixel_size`, so saving a window and restoring it any number of times never
// grows it. At a fractional scale the reported size rounds up and restores to
// more pixels than the window had. Returns `dip_bounds` unchanged when it is
// empty, `pixel_size` is empty, `scale` is not above zero, or no size within
// a few DIP of the reported one fits.
gfx::Rect StableWindowPlacementBounds(const gfx::Rect& dip_bounds,
                                      const gfx::Size& pixel_size,
                                      float scale);

// The pixel size `dip_bounds` occupies when restored at `scale`.
gfx::Size RestoredPixelSize(const gfx::Rect& dip_bounds, float scale);

}  // namespace custom_browser

#endif  // CUSTOM_BROWSER_UI_VIEW_WINDOW_PLACEMENT_STABILITY_H_
