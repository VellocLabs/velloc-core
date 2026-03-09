#include "custom_browser/ui/view/custom_browser_view_layout_delegate.h"

#include "chrome/browser/ui/find_bar/find_bar.h"
#include "chrome/browser/ui/find_bar/find_bar_controller.h"
#include "chrome/browser/ui/views/frame/tab_strip_region_view.h"
#include "chrome/browser/ui/views/web_apps/frame_toolbar/web_app_frame_toolbar_view.h"
#include "custom_browser/ui/view/custom_browser_view.h"

CustomBrowserViewDelegateImpl::CustomBrowserViewDelegateImpl(
    CustomBrowserView* custom_browser_view)
    : custom_browser_view_(custom_browser_view) {}

CustomBrowserViewDelegateImpl::~CustomBrowserViewDelegateImpl() = default;

bool CustomBrowserViewDelegateImpl::ShouldDrawTabStrip() const {
  return custom_browser_view_->ShouldDrawTabStrip();
}

bool CustomBrowserViewDelegateImpl::GetBorderlessModeEnabled() const {
  return custom_browser_view_->IsBorderlessModeEnabled();
}

gfx::Rect
CustomBrowserViewDelegateImpl::GetBoundsForTabStripRegionInBrowserView() const {
  const gfx::Size tabstrip_minimum_size =
      custom_browser_view_->tab_strip_view()->GetMinimumSize();
  gfx::RectF bounds_f(custom_browser_view_->frame()->GetBoundsForTabStripRegion(
      tabstrip_minimum_size));
  views::View::ConvertRectToTarget(custom_browser_view_->parent(),
                                   custom_browser_view_, &bounds_f);
  return gfx::ToEnclosingRect(bounds_f);
}

gfx::Rect
CustomBrowserViewDelegateImpl::GetBoundsForWebAppFrameToolbarInBrowserView()
    const {
  const gfx::Size web_app_frame_toolbar_preferred_size =
      custom_browser_view_->web_app_frame_toolbar()->GetPreferredSize();
  gfx::RectF bounds_f(
      custom_browser_view_->frame()->GetBoundsForWebAppFrameToolbar(
          web_app_frame_toolbar_preferred_size));
  views::View::ConvertRectToTarget(custom_browser_view_->parent(),
                                   custom_browser_view_, &bounds_f);
  return gfx::ToEnclosingRect(bounds_f);
}

int CustomBrowserViewDelegateImpl::GetTopInsetInBrowserView() const {
  if (custom_browser_view_->IsWindowControlsOverlayEnabled() ||
      custom_browser_view_->IsBorderlessModeEnabled()) {
    return 0;
  }
#if BUILDFLAG(IS_MAC)
  if (custom_browser_view_->UsesImmersiveFullscreenTabbedMode() &&
      custom_browser_view_->immersive_mode_controller()->IsEnabled()) {
    return 0;
  }
#endif

  return custom_browser_view_->frame()->GetTopInset() -
         custom_browser_view_->y();
}

bool CustomBrowserViewDelegateImpl::IsToolbarVisible() const {
  return custom_browser_view_->IsToolbarVisible();
}

bool CustomBrowserViewDelegateImpl::IsBookmarkBarVisible() const {
  return custom_browser_view_->IsBookmarkBarVisible();
}

bool CustomBrowserViewDelegateImpl::IsContentsSeparatorEnabled() const {
  return !custom_browser_view_->browser()->app_controller() &&
         !IsActiveTabSplit();
}

bool CustomBrowserViewDelegateImpl::IsActiveTabSplit() const {
  return custom_browser_view_->browser()->tab_strip_model()->IsActiveTabSplit();
}

const ImmersiveModeController*
CustomBrowserViewDelegateImpl::GetImmersiveModeController() const {
  return custom_browser_view_->immersive_mode_controller();
}

ExclusiveAccessBubbleViews*
CustomBrowserViewDelegateImpl::GetExclusiveAccessBubble() const {
  return custom_browser_view_->exclusive_access_bubble();
}

bool CustomBrowserViewDelegateImpl::IsTopControlsSlideBehaviorEnabled() const {
  return custom_browser_view_->GetTopControlsSlideBehaviorEnabled();
}

float CustomBrowserViewDelegateImpl::GetTopControlsSlideBehaviorShownRatio()
    const {
  return custom_browser_view_->GetTopControlsSlideBehaviorShownRatio();
}

bool CustomBrowserViewDelegateImpl::SupportsWindowFeature(
    Browser::WindowFeature feature) const {
  return custom_browser_view_->browser()->SupportsWindowFeature(feature);
}

gfx::NativeView CustomBrowserViewDelegateImpl::GetHostViewForAnchoring() const {
  return custom_browser_view_->GetWidgetForAnchoring()->GetNativeView();
}

bool CustomBrowserViewDelegateImpl::HasFindBarController() const {
  return custom_browser_view_->browser()->GetFeatures().HasFindBarController();
}

void CustomBrowserViewDelegateImpl::MoveWindowForFindBarIfNecessary() const {
  auto* const controller =
      custom_browser_view_->browser()->GetFeatures().GetFindBarController();
  return controller->find_bar()->MoveWindowIfNecessary();
}

bool CustomBrowserViewDelegateImpl::IsWindowControlsOverlayEnabled() const {
  return custom_browser_view_->IsWindowControlsOverlayEnabled();
}

void CustomBrowserViewDelegateImpl::UpdateWindowControlsOverlay(
    const gfx::Rect& available_titlebar_area) {
  content::WebContents* web_contents =
      custom_browser_view_->GetActiveWebContents();
  if (!web_contents) {
    return;
  }

  web_contents->UpdateWindowControlsOverlay(
      available_titlebar_area.IsEmpty()
          ? gfx::Rect()
          : custom_browser_view_->GetMirroredRect(available_titlebar_area));
}

bool CustomBrowserViewDelegateImpl::ShouldLayoutTabStrip() const {
#if BUILDFLAG(IS_MAC)
  if (custom_browser_view_->UsesImmersiveFullscreenTabbedMode() &&
      custom_browser_view_->immersive_mode_controller()->IsEnabled()) {
    return false;
  }
#endif
  return true;
}

int CustomBrowserViewDelegateImpl::GetExtraInfobarOffset() const {
#if BUILDFLAG(IS_MAC)
  if (custom_browser_view_->UsesImmersiveFullscreenMode() &&
      custom_browser_view_->immersive_mode_controller()->IsEnabled()) {
    return custom_browser_view_->immersive_mode_controller()
        ->GetExtraInfobarOffset();
  }
#endif
  return 0;
}
