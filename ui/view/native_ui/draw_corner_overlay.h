#ifndef CUSTOM_BROWSER_UI_VIEW_DRAW_CORNER_OVERLAY_H_
#define CUSTOM_BROWSER_UI_VIEW_DRAW_CORNER_OVERLAY_H_

#include <array>
#include <optional>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/color/color_id.h"
#include "ui/views/view.h"
#include "ui/views/view_observer.h"


namespace gfx {
class Rect;
class Size;
class Canvas;
}  // namespace gfx

class BrowserView;  // fwd decl

namespace custom_browser::native_ui {

// DrawCornerOverlay:
//  - Adds small overlay Views to visually "round" selected corners of |target|
//  - Overlays paint TopContainerBackground + curved separator stroke
//  - Auto-removes itself when |target| is destroyed
//
// Usage:
//   auto* overlay = DrawCornerOverlay::Create(target_view,
//                    DrawCornerOverlay::kTopLeft |
//                    DrawCornerOverlay::kTopRight);
//
// Notes:
//   - Overlays are children of target->parent() and z-ordered after |target|.
//   - Corners' radius uses the standard token
//       views::ShapeContextTokens::kSidePanelPageContentRadius
//     (change if your design uses a different token).
//   - This ONLY covers the corners. If you also want straight-edge
//     separators, add your own thin strip views (optional).
class CornerView;

class DrawCornerOverlay : public views::ViewObserver {
 public:
  enum CornerFlags : uint32_t {
    kNone = 0,
    kTopLeft = 1u << 0,
    kTopRight = 1u << 1,
    kBottomRight = 1u << 2,
    kBottomLeft = 1u << 3,
    kAll = kTopLeft | kTopRight | kBottomRight | kBottomLeft,
  };

  // Creates, installs.
  static std::unique_ptr<DrawCornerOverlay> Create(views::View* browser_view,
                                                   views::View* host_view,
                                                   uint32_t corner_flags);

  // Optional: update which corners are shown.
  void SetCornerFlags(uint32_t corner_flags);

  // Optional: force relayout (e.g., if external insets changed).
  void Relayout(const gfx::Rect* force_bounds);

  void SetCornerRadius(int radius);

  DrawCornerOverlay(views::View* browser_view, views::View* host_view, uint32_t flags);
  ~DrawCornerOverlay() override;

  // views::ViewObserver:
  void OnViewBoundsChanged(views::View* observed_view) override;
  void OnViewVisibilityChanged(views::View* observed_view,
                               views::View* starting_view,
                               bool visible) override;
  void OnViewAddedToWidget(views::View* observed_view) override;
  void OnViewIsDeleting(views::View* observed_view) override;

  void EnsureInstalled();
  void CreateOrUpdateCornerViews();
  void UpdateCornerBoundsAndVisibility(const gfx::Rect* force_bounds = nullptr);
  void RemoveCornerViews();

  // Optional: override the separator stroke and corner fill using a ColorId.
  // Pass std::nullopt to reset to themed colors
  // (stroke uses kColorToolbarContentAreaSeparator; fill uses background).
  // Example:
  // SetSeparatorColor(custom_browser::mojom::ChromeColorID::kColorWebUiTabStripBackground)
  // after converting to a ui::ColorId.
  void SetSeparatorColor(std::optional<ui::ColorId> color_id);

  raw_ptr<views::View> browser_view_;
  raw_ptr<views::View> host_view_;
  uint32_t flags_ = kNone;

  // Order: TL, TR, BR, BL
  std::array<CornerView*, 4> corners_ = {nullptr, nullptr, nullptr, nullptr};

  int corner_radius_dip_ = 5;
  std::optional<ui::ColorId> separator_color_override_id_;
  base::WeakPtrFactory<DrawCornerOverlay> weak_factory_{this};
};

}  // namespace custom_browser::native_ui

#endif  // CUSTOM_BROWSER_UI_VIEW_DRAW_CORNER_OVERLAY_H_
