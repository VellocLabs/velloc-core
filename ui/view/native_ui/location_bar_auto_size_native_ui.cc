#include "custom_browser/ui/view/native_ui/location_bar_auto_size_native_ui.h"

#include <algorithm>
#include <cmath>

#include "base/time/time.h"
#include "chrome/browser/ui/views/frame/multi_contents_view.h"
#include "chrome/browser/ui/views/location_bar/location_bar_view.h"
#include "chrome/browser/ui/views/omnibox/omnibox_view_views.h"
#include "custom_browser/ui/mojom/browser_constants.mojom-forward.h"
#include "custom_browser/ui/view/custom_browser_view.h"
#include "ui/gfx/geometry/size.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/view.h"


namespace custom_browser::native_ui {

namespace {

// constexpr int kWidthAnimationDurationMs = 150;

}  // namespace

LocationBarAutoSizeNativeUI::LocationBarAutoSizeNativeUI(
    views::View* host_view,
    const custom_browser::mojom::NativeViewParams* params)
    : NativeUI(host_view, params) {}

LocationBarAutoSizeNativeUI::~LocationBarAutoSizeNativeUI() {
  ResetFocusObservation();
  LocationBarView* location_bar = location_bar_state_.view();
  location_bar_state_.Reset();
  if (location_bar) {
    location_bar->SetVisible(true);
  }
  // Prevent NativeUI::~NativeUI() from hiding the restored view.
  if (!views_.empty()) {
    views_[0] = nullptr;
  }
  // rect_animation_.reset();
}

void LocationBarAutoSizeNativeUI::OnPrepare() {
  DCHECK(!view());
  auto* location_bar = GetCustomBrowserView()->GetLocationBarView();
  location_bar_state_ = location_bar;
  location_bar_state_.SetParent(host_view()->parent());
  AddView(location_bar);
  RefreshExpandOnFocusState();
}

void LocationBarAutoSizeNativeUI::OnLayout() {
  MaybeApplyFocusState();
}

void LocationBarAutoSizeNativeUI::OnDidChangeFocus(views::View* focused_before,
                                                   views::View* focused_now) {
  MaybeApplyFocusState();
}

void LocationBarAutoSizeNativeUI::RefreshExpandOnFocusState() {
  EnsureFocusObservation();
  MaybeApplyFocusState();
}

void LocationBarAutoSizeNativeUI::EnsureFocusObservation() {
  if (!expand_on_focus_enabled_) {
    return;
  }
  views::FocusManager* focus_manager = view()->GetFocusManager();
  if (!focus_manager) {
    return;
  }

  if (focus_manager_ == focus_manager) {
    return;
  }

  ResetFocusObservation();
  focus_manager_ = focus_manager;
  focus_manager_->AddFocusChangeListener(this);
}

void LocationBarAutoSizeNativeUI::ResetFocusObservation() {
  if (!focus_manager_) {
    return;
  }

  focus_manager_->RemoveFocusChangeListener(this);
  focus_manager_ = nullptr;
}

void LocationBarAutoSizeNativeUI::MaybeApplyFocusState() {
  UpdateViewsVisibility();
  if (!visible()) {
    return;
  }
  auto target_bounds = bounds();
  EnsureFocusObservation();
  if (expand_on_focus_enabled_) {
    // If the focus is currently in the omnibox, expand the address box.
    OmniboxViewViews* omnibox = GetOmniboxTextField();
    if (omnibox && omnibox->HasFocus()) {
      auto host_bounds = host_view_->GetLocalBounds();
      int expanded_width = (host_bounds.width() - target_bounds.x()) / 2;
      expanded_width = std::max(expanded_width, target_bounds.width());
      target_bounds.set_width(expanded_width);
    }
  }
  view()->SetBoundsRect(target_bounds);
}

// void LocationBarAutoSizeNativeUI::ExpandAddressBox() {
//   AnimateToRect(GetFloatingTargetAddressBounds());
// }

// void LocationBarAutoSizeNativeUI::RestoreAddressBox() {
//   AnimateToRect(bounds());
// }
// gfx::Rect LocationBarAutoSizeNativeUI::GetFloatingTargetAddressBounds() const
// {
//   auto target_bounds = bounds();
//   target_bounds.set_width(kFocusedAddressBoxWidthPx);
//   auto* multi_contents_view = GetCustomBrowserView()->multi_contents_view();
//   DCHECK(multi_contents_view);
//   if (!multi_contents_view->IsInSplitView()) {
//     return target_bounds;
//   }

//   auto* active_contents_container =
//   multi_contents_view->GetActiveContentsContainerView(); auto
//   active_contents_container_rect = active_contents_container->bounds(); auto
//   contents_rect = views::View::ConvertRectToTarget(multi_contents_view,
//   RealParent(), active_contents_container_rect);
//   target_bounds.set_x(contents_rect.x());
//   target_bounds.set_y(contents_rect.y());
//   return target_bounds;
// }

// void LocationBarAutoSizeNativeUI::AnimateToRect(const gfx::Rect&
// target_bounds) {
//   if (animation_target_bounds_ == target_bounds && rect_animation_) {
//     return;
//   }

//   animation_start_bounds_ = GetLocationBarView()->bounds();
//   animation_target_bounds_ = target_bounds;
//   if (!rect_animation_) {
//     rect_animation_ = std::make_unique<gfx::SlideAnimation>(this);
//     rect_animation_->SetTweenType(gfx::Tween::FAST_OUT_SLOW_IN);
//     rect_animation_->SetSlideDuration(base::Milliseconds(kWidthAnimationDurationMs));
//   }

//   rect_animation_->Reset(0.0);
//   rect_animation_->Show();
// }

// void LocationBarAutoSizeNativeUI::ApplyWidthImmediate(const gfx::Rect&
// bounds) {
//   GetLocationBarView()->SetBoundsRect(bounds);
//   GetLocationBarView()->InvalidateLayout();
//   GetLocationBarView()->SchedulePaint();
// }

// void LocationBarAutoSizeNativeUI::AnimationProgressed(const gfx::Animation*
// animation) {
//   if (!rect_animation_ || animation != rect_animation_.get())
//     return;

//   const double progress = static_cast<const
//   gfx::SlideAnimation*>(animation)->GetCurrentValue(); gfx::Rect next_bounds
//   = animation_start_bounds_; const int delta_x = animation_target_bounds_.x()
//   - animation_start_bounds_.x(); const int delta_y =
//   animation_target_bounds_.y() - animation_start_bounds_.y(); const int
//   delta_width = animation_target_bounds_.width() -
//   animation_start_bounds_.width(); const int delta_height =
//   animation_target_bounds_.height() - animation_start_bounds_.height();
//   next_bounds.set_x(animation_start_bounds_.x() +
//                     static_cast<int>(std::lround(delta_x * progress)));
//   next_bounds.set_y(animation_start_bounds_.y() +
//                     static_cast<int>(std::lround(delta_y * progress)));
//   next_bounds.set_width(animation_start_bounds_.width() +
//                         static_cast<int>(std::lround(delta_width *
//                         progress)));
//   next_bounds.set_height(animation_start_bounds_.height() +
//                          static_cast<int>(std::lround(delta_height *
//                          progress)));
//   ApplyWidthImmediate(next_bounds);
// }

// void LocationBarAutoSizeNativeUI::AnimationEnded(const gfx::Animation*
// animation) {
//   if (!rect_animation_ || animation != rect_animation_.get())
//     return;
//   // rect_animation_.reset();
//   ApplyWidthImmediate(animation_target_bounds_);
// }

// void LocationBarAutoSizeNativeUI::AnimationCanceled(const gfx::Animation*
// animation) {
//   AnimationEnded(animation);
// }

LocationBarView* LocationBarAutoSizeNativeUI::GetLocationBarView() const {
  return static_cast<LocationBarView*>(view());
}

OmniboxViewViews* LocationBarAutoSizeNativeUI::GetOmniboxTextField() const {
  return GetLocationBarView()->omnibox_view();
}

}  // namespace custom_browser::native_ui
