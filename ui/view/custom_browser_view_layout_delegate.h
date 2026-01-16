#ifndef CUSTOM_BROWSER_UI_CUSTOM_BROWSER_VIEW_LAYOUT_DELEGATE_H_
#define CUSTOM_BROWSER_UI_CUSTOM_BROWSER_VIEW_LAYOUT_DELEGATE_H_

#include "chrome/browser/ui/views/frame/layout/browser_view_layout_delegate.h"
class CustomBrowserView;

class CustomBrowserViewDelegateImpl : public BrowserViewLayoutDelegate {
 public:
  explicit CustomBrowserViewDelegateImpl(CustomBrowserView* custom_browser_view);
  ~CustomBrowserViewDelegateImpl() override;

  // All overrides forward to the original
  bool ShouldDrawTabStrip() const override;
  bool GetBorderlessModeEnabled() const override;
  gfx::Rect GetBoundsForTabStripRegionInBrowserView() const override;
  gfx::Rect GetBoundsForWebAppFrameToolbarInBrowserView() const override;
  int GetTopInsetInBrowserView() const override;
  bool IsToolbarVisible() const override;
  bool IsBookmarkBarVisible() const override;
  bool IsContentsSeparatorEnabled() const override;
  bool IsActiveTabSplit() const override;
  const ImmersiveModeController* GetImmersiveModeController() const override;
  ExclusiveAccessBubbleViews* GetExclusiveAccessBubble() const override;
  bool IsTopControlsSlideBehaviorEnabled() const override;
  float GetTopControlsSlideBehaviorShownRatio() const override;
  bool SupportsWindowFeature(Browser::WindowFeature feature) const override;
  gfx::NativeView GetHostViewForAnchoring() const override;
  bool HasFindBarController() const override;
  void MoveWindowForFindBarIfNecessary() const override;
  bool IsWindowControlsOverlayEnabled() const override;
  void UpdateWindowControlsOverlay(const gfx::Rect& available_titlebar_area) override;
  bool ShouldLayoutTabStrip() const override;
  int GetExtraInfobarOffset() const override;

 private:
  raw_ptr<CustomBrowserView> custom_browser_view_;
};

#endif  // CUSTOM_BROWSER_UI_CUSTOM_BROWSER_VIEW_LAYOUT_DELEGATE_H_
