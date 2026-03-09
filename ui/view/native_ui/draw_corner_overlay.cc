#include "custom_browser/ui/view/native_ui/draw_corner_overlay.h"

#include <algorithm>
#include <memory>

#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/side_panel/side_panel.h"
#include "third_party/skia/include/core/SkPathBuilder.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/ui_base_features.h"
#include "ui/compositor/layer.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/scoped_canvas.h"
#include "ui/views/controls/separator.h"
#include "ui/views/layout/layout_provider.h"


namespace custom_browser::native_ui {

namespace {

// Which corner index in the array.
enum CornerIndex { kIdxTL = 0, kIdxTR = 1, kIdxBR = 2, kIdxBL = 3 };
constexpr int kDefaultCornerRadiusDip = 5;
constexpr int kSeparatorThicknessDip = views::Separator::kThickness;
}  // namespace

// ------------------------ CornerView ---------------------------------

// Internal view used by DrawCornerOverlay. Kept in this TU mirroring
// patterns like ScrollCornerView in scroll_view.cc.
class CornerView : public views::View {
  METADATA_HEADER(CornerView, View)

 public:
  explicit CornerView(int which_index) : which_(which_index) {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    SetCanProcessEventsWithinSubtree(false);
  }

  void UpdateWhich(int which_index) {
    which_ = which_index;
    SchedulePaint();
  }

  void SetCornerRadius(int radius_dip) {
    const int new_radius = std::max(0, radius_dip);
    if (radius_dip_ == new_radius) {
      return;
    }
    radius_dip_ = new_radius;
    InvalidateLayout();
    SchedulePaint();
  }

  void SetCustomColorId(std::optional<ui::ColorId> color_id) {
    custom_color_id_ = color_id;
    SchedulePaint();
  }

  gfx::Size GetPreferredSize() const {
    const int radius = std::max(0, radius_dip_);
    return gfx::Size(radius + kSeparatorThicknessDip,
                     radius + kSeparatorThicknessDip);
  }

 protected:
  void OnBoundsChanged(const gfx::Rect& previous_bounds) override {
    const gfx::Rect lb = GetLocalBounds();
    const float w = static_cast<float>(lb.width());
    const float h = static_cast<float>(lb.height());
    const float r = static_cast<float>(std::max(0, radius_dip_));
    const float t = static_cast<float>(kSeparatorThicknessDip);

    SkPathBuilder path;
    switch (static_cast<CornerIndex>(which_)) {
      case kIdxTL: {
        path.moveTo(w, 0);
        path.lineTo(0, 0);
        path.lineTo(0, h);
        path.lineTo(t, h);
        path.arcTo({r, r}, 0, SkPathBuilder::kSmall_ArcSize,
                   SkPathDirection::kCW, {w, t});
        path.lineTo(w, 0);
        break;
      }
      case kIdxTR: {
        path.moveTo(0, 0);
        path.lineTo(w, 0);
        path.lineTo(w, h);
        path.lineTo(w - t, h);
        // Mirror of TL: use CCW to sweep the inner arc correctly for TR.
        path.arcTo({r, r}, 0, SkPathBuilder::kSmall_ArcSize,
                   SkPathDirection::kCCW, {0, t});
        path.lineTo(0, 0);
        break;
      }
      case kIdxBR: {
        path.moveTo(0, h);
        path.lineTo(w, h);
        path.lineTo(w, 0);
        path.lineTo(w - t, 0);
        path.arcTo({r, r}, 0, SkPathBuilder::kSmall_ArcSize,
                   SkPathDirection::kCW, {0, h - t});
        path.lineTo(0, h);
        break;
      }
      case kIdxBL: {
        path.moveTo(0, 0);
        path.lineTo(0, h);
        path.lineTo(w, h);
        path.lineTo(w, h - t);
        // Use CW for bottom-left to mirror bottom-right and fix direction.
        path.arcTo({r, r}, 0, SkPathBuilder::kSmall_ArcSize,
                   SkPathDirection::kCW, {t, 0});
        path.lineTo(0, 0);
        break;
      }
    }
    SetClipPath(path.detach());
  }

  void OnPaint(gfx::Canvas* canvas) override {
    views::View::OnPaint(canvas);

    // If a custom separator color is provided (via ColorId), also fill the
    // clipped background area with this color so the background matches the
    // stroke.
    if (custom_color_id_.has_value()) {
      const SkColor fill =
          GetColorProvider()->GetColor(custom_color_id_.value());
      // The view has a clip path set in OnBoundsChanged(), so filling the
      // rect here will only paint inside the rounded-corner wedge.
      canvas->FillRect(GetLocalBounds(), fill);
    }

    cc::PaintFlags flags;
    flags.setStrokeWidth(kSeparatorThicknessDip * 2);
    const SkColor stroke =
        custom_color_id_.has_value()
            ? GetColorProvider()->GetColor(custom_color_id_.value())
            : GetColorProvider()->GetColor(kColorToolbarContentAreaSeparator);
    flags.setColor(stroke);
    flags.setStyle(cc::PaintFlags::kStroke_Style);
    flags.setAntiAlias(true);

    const gfx::Rect lb = GetLocalBounds();
    const float w = static_cast<float>(lb.width());
    const float h = static_cast<float>(lb.height());
    const float r = static_cast<float>(std::max(0, radius_dip_));
    const float t = static_cast<float>(kSeparatorThicknessDip);

    SkPathBuilder arc;
    switch (static_cast<CornerIndex>(which_)) {
      case kIdxTL: {
        arc.moveTo(t, h);
        arc.arcTo({r, r}, 0, SkPathBuilder::kSmall_ArcSize,
                  SkPathDirection::kCW, {w, t});
        break;
      }
      case kIdxTR: {
        arc.moveTo(w - t, h);
        arc.arcTo({r, r}, 0, SkPathBuilder::kSmall_ArcSize,
                  SkPathDirection::kCCW, {0, t});
        break;
      }
      case kIdxBR: {
        arc.moveTo(w - t, 0);
        arc.arcTo({r, r}, 0, SkPathBuilder::kSmall_ArcSize,
                  SkPathDirection::kCW, {0, h - t});
        break;
      }
      case kIdxBL: {
        arc.moveTo(t, 0);
        arc.arcTo({r, r}, 0, SkPathBuilder::kSmall_ArcSize,
                  SkPathDirection::kCW, {t, h});
        break;
      }
    }
    canvas->DrawPath(arc.detach(), flags);
  }

 private:
  int which_ = 0;
  int radius_dip_ = kDefaultCornerRadiusDip;
  std::optional<ui::ColorId> custom_color_id_;
};

BEGIN_METADATA(CornerView)
END_METADATA

// ------------------------ DrawCornerOverlay ---------------------------

std::unique_ptr<DrawCornerOverlay> DrawCornerOverlay::Create(
    views::View* browser_view,
    views::View* host_view,
    uint32_t corner_flags) {
  auto* self = new DrawCornerOverlay(browser_view, host_view, corner_flags);
  self->EnsureInstalled();
  return std::unique_ptr<DrawCornerOverlay>(self);
}

DrawCornerOverlay::DrawCornerOverlay(views::View* browser_view,
                                     views::View* host_view,
                                     uint32_t flags)
    : browser_view_(browser_view), host_view_(host_view), flags_(flags) {
  CHECK(browser_view_);
  host_view_->AddObserver(this);
}

DrawCornerOverlay::~DrawCornerOverlay() {
  if (host_view_) {
    host_view_->RemoveObserver(this);
  }
  RemoveCornerViews();
}

void DrawCornerOverlay::OnViewBoundsChanged(views::View* observed_view) {
  UpdateCornerBoundsAndVisibility();
}

void DrawCornerOverlay::OnViewVisibilityChanged(views::View* observed_view,
                                                views::View* starting_view,
                                                bool visible) {
  UpdateCornerBoundsAndVisibility();
}

void DrawCornerOverlay::OnViewAddedToWidget(views::View* observed_view) {
  EnsureInstalled();
  UpdateCornerBoundsAndVisibility();
}

void DrawCornerOverlay::OnViewIsDeleting(views::View* observed_view) {
  // Target is going away; tear everything down and delete self.
  RemoveCornerViews();
  if (host_view_) {
    host_view_->RemoveObserver(this);
  }
  host_view_ = nullptr;
}

void DrawCornerOverlay::SetCornerFlags(uint32_t corner_flags) {
  if (flags_ == corner_flags) {
    return;
  }
  flags_ = corner_flags;
  CreateOrUpdateCornerViews();
  UpdateCornerBoundsAndVisibility();
}

void DrawCornerOverlay::Relayout(const gfx::Rect* force_bounds) {
  CreateOrUpdateCornerViews();
  UpdateCornerBoundsAndVisibility(force_bounds);
}

void DrawCornerOverlay::SetCornerRadius(int radius) {
  radius = std::max(0, radius);
  if (corner_radius_dip_ == radius) {
    return;
  }
  corner_radius_dip_ = radius;
  CreateOrUpdateCornerViews();
  UpdateCornerBoundsAndVisibility();
}

void DrawCornerOverlay::EnsureInstalled() {
  if (!browser_view_) {
    return;
  }

  // Create corner views if needed.
  CreateOrUpdateCornerViews();
  // Make sure they are stacked after (above) the target.
  for (auto* cv : corners_) {
    if (!cv) {
      continue;
    }
    if (cv->parent() == browser_view_) {
      browser_view_->ReorderChildView(cv, browser_view_->children().size() - 1);
    }
  }
}

void DrawCornerOverlay::CreateOrUpdateCornerViews() {
  if (!browser_view_) {
    return;
  }

  const auto ensure_corner = [&](CornerIndex idx, bool needed) {
    if (needed && !corners_[idx]) {
      auto* cv = browser_view_->AddChildView(std::make_unique<CornerView>(idx));
      cv->SetCornerRadius(corner_radius_dip_);
      if (separator_color_override_id_.has_value()) {
        cv->SetCustomColorId(separator_color_override_id_);
      }
      corners_[idx] = cv;
    } else if (!needed && corners_[idx]) {
      browser_view_->RemoveChildViewT(corners_[idx]);
      corners_[idx] = nullptr;
    } else if (needed && corners_[idx]) {
      corners_[idx]->UpdateWhich(idx);  // no-op but explicit
      corners_[idx]->SetCornerRadius(corner_radius_dip_);
    }
  };

  ensure_corner(kIdxTL, (flags_ & kTopLeft) != 0);
  ensure_corner(kIdxTR, (flags_ & kTopRight) != 0);
  ensure_corner(kIdxBR, (flags_ & kBottomRight) != 0);
  ensure_corner(kIdxBL, (flags_ & kBottomLeft) != 0);
}

void DrawCornerOverlay::UpdateCornerBoundsAndVisibility(
    const gfx::Rect* force_bounds) {
  if (!browser_view_) {
    return;
  }

  // In parent coords (same coord-space we need).
  gfx::Rect target_bounds =
      force_bounds ? *force_bounds : browser_view_->bounds();
  target_bounds.Outset(kSeparatorThicknessDip);
  const int r = std::max(0, corner_radius_dip_);
  const int t = kSeparatorThicknessDip;
  const int s = r + t;  // overlay square size

  const auto set_bounds = [&](CornerIndex idx, int x, int y) {
    if (!corners_[idx]) {
      return;
    }
    corners_[idx]->SetBounds(x, y, s, s);
    corners_[idx]->SetVisible(host_view_->GetVisible());
  };

  // Position small squares at each requested corner.
  if (corners_[kIdxTL]) {
    set_bounds(kIdxTL, target_bounds.x(), target_bounds.y());
  }
  if (corners_[kIdxTR]) {
    set_bounds(kIdxTR, target_bounds.right() - s, target_bounds.y());
  }
  if (corners_[kIdxBR]) {
    set_bounds(kIdxBR, target_bounds.right() - s, target_bounds.bottom() - s);
  }
  if (corners_[kIdxBL]) {
    set_bounds(kIdxBL, target_bounds.x(), target_bounds.bottom() - s);
  }
}

void DrawCornerOverlay::RemoveCornerViews() {
  // If target/parent is in teardown, avoid dereferencing potentially
  // invalid child pointers; parent will delete its children.
  for (auto& ptr : corners_) {
    if (!ptr) {
      continue;
    }
    if (browser_view_) {
      browser_view_->RemoveChildViewT(ptr);
    }
    ptr = nullptr;
  }
}

void DrawCornerOverlay::SetSeparatorColor(std::optional<ui::ColorId> color_id) {
  separator_color_override_id_ = color_id;
  for (auto* cv : corners_) {
    if (!cv) {
      continue;
    }
    cv->SetCustomColorId(color_id);
  }
}

}  // namespace custom_browser::native_ui
