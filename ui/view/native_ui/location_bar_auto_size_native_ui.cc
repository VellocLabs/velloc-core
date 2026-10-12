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

LocationBarView* LocationBarAutoSizeNativeUI::GetLocationBarView() const {
  return static_cast<LocationBarView*>(view());
}

OmniboxViewViews* LocationBarAutoSizeNativeUI::GetOmniboxTextField() const {
  return GetLocationBarView()->omnibox_view();
}

}  // namespace custom_browser::native_ui
