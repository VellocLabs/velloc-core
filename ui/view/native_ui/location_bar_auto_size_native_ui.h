#ifndef CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_LOCATION_BAR_AUTO_SIZE_NATIVE_UI_H_
#define CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_LOCATION_BAR_AUTO_SIZE_NATIVE_UI_H_

#include <memory>
#include <optional>
#include <string_view>

#include "base/memory/raw_ptr.h"
#include "chrome/browser/ui/views/location_bar/location_bar_view.h"
#include "custom_browser/ui/mojom/browser_constants.mojom-forward.h"
#include "custom_browser/ui/view/native_ui/native_ui.h"
#include "custom_browser/ui/view/native_ui/native_ui_registration_macros.h"
#include "custom_browser/ui/view/native_ui/scoped_view_state.h"
#include "ui/gfx/animation/animation_delegate.h"
#include "ui/gfx/animation/slide_animation.h"
#include "ui/gfx/animation/tween.h"
#include "ui/views/focus/focus_manager.h"


namespace views {
class FocusManager;
}  // namespace views

class LocationBarView;
class OmniboxViewViews;

// Native wrapper for the LocationBarView that handles focus-based width
// expansion driven by the layout extraStyle flag.
namespace custom_browser::native_ui {

class LocationBarAutoSizeNativeUI : public NativeUI,
                                    public views::FocusChangeListener,
                                    public gfx::AnimationDelegate {
 public:
  LocationBarAutoSizeNativeUI(
      views::View* host_view,
      const custom_browser::mojom::NativeViewParams* params);
  LocationBarAutoSizeNativeUI(const LocationBarAutoSizeNativeUI&) = delete;
  LocationBarAutoSizeNativeUI& operator=(const LocationBarAutoSizeNativeUI&) =
      delete;
  ~LocationBarAutoSizeNativeUI() override;

  DECLARE_NATIVE_UI_CLASS_NAME(
      LocationBarAutoSizeNativeUI,
      custom_browser::mojom::kLocationBarAutoSizeViewName)

 protected:
  void OnPrepare() override;
  void OnLayout() override;

  // views::FocusChangeListener
  void OnWillChangeFocus(views::View* focused_before,
                         views::View* focused_now) override {}
  void OnDidChangeFocus(views::View* focused_before,
                        views::View* focused_now) override;

 private:
  bool ShouldExpandOnFocus() const;
  void RefreshExpandOnFocusState();
  void EnsureFocusObservation();
  void ResetFocusObservation();
  void MaybeApplyFocusState();
  // void ExpandAddressBox();
  // void RestoreAddressBox();
  // void AnimateToRect(const gfx::Rect& target_bounds);
  // void ApplyWidthImmediate(const gfx::Rect& bounds);

  // gfx::Rect GetFloatingTargetAddressBounds() const;
  LocationBarView* GetLocationBarView() const;
  OmniboxViewViews* GetOmniboxTextField() const;
  // void AnimationProgressed(const gfx::Animation* animation) override;
  // void AnimationEnded(const gfx::Animation* animation) override;
  // void AnimationCanceled(const gfx::Animation* animation) override;

  bool expand_on_focus_enabled_ = true;
  raw_ptr<views::FocusManager> focus_manager_ = nullptr;
  ScopedViewState<LocationBarView> location_bar_state_;
  // std::unique_ptr<gfx::SlideAnimation> rect_animation_;
  // gfx::Rect animation_start_bounds_;
  // gfx::Rect animation_target_bounds_;
};

}  // namespace custom_browser::native_ui

#endif  // CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_LOCATION_BAR_AUTO_SIZE_NATIVE_UI_H_
