#ifndef CUSTOM_BROWSER_UI_CUSTOM_BROWSER_VIEW_H_
#define CUSTOM_BROWSER_UI_CUSTOM_BROWSER_VIEW_H_

#include "chrome/browser/ui/views/frame/browser_view.h"


#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/callback_list.h"
#include "base/functional/callback.h"
#include "third_party/blink/public/mojom/page/draggable_region.mojom.h"
#include "base/gtest_prod_util.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/scoped_observation.h"
#include "base/task/cancelable_task_tracker.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "build/branding_buildflags.h"
#include "build/build_config.h"
#include "chrome/browser/extensions/extension_commands_global_registry.h"
#include "chrome/browser/extensions/extension_keybinding_registry.h"
#include "chrome/browser/ui/browser.h"
#include "components/tabs/public/tab_interface.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/exclusive_access/exclusive_access_context.h"
#include "chrome/browser/ui/exclusive_access/fullscreen_observer.h"
#include "chrome/browser/ui/tabs/tab_data.h"
#include "chrome/browser/ui/tabs/tab_strip_model_observer.h"
#include "chrome/browser/ui/translate/partial_translate_bubble_model.h"
#include "chrome/browser/ui/user_education/browser_user_education_interface.h"
#include "chrome/browser/ui/views/exclusive_access_bubble_views_context.h"
#include "chrome/browser/ui/views/extensions/extension_keybinding_registry_views.h"
#include "chrome/browser/ui/views/frame/contents_container_view.h"
#include "chrome/browser/ui/views/frame/contents_web_view.h"
#include "chrome/browser/ui/views/frame/immersive_mode_controller.h"
#include "chrome/browser/ui/views/intent_picker_bubble_view.h"
#include "chrome/browser/ui/views/omnibox/omnibox_popup_closer.h"
#include "chrome/browser/ui/views/tabs/tab.h"
#include "chrome/browser/ui/webui/tab_search/tab_search.mojom.h"
#include "chrome/common/buildflags.h"
#include "components/enterprise/buildflags/buildflags.h"
#include "components/infobars/core/infobar_container.h"
#include "components/user_education/common/feature_promo/feature_promo_handle.h"
#include "components/webapps/browser/banners/app_banner_manager.h"
#include "content/public/browser/page_user_data.h"
#include "content/public/browser/permission_controller.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "third_party/blink/public/common/permissions/permission_utils.h"
#include "ui/base/accelerators/accelerator.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/base/mojom/window_show_state.mojom-forward.h"
#include "ui/base/pointer/touch_ui_controller.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/views/controls/button/button.h"
#include "ui/views/controls/webview/unhandled_keyboard_event_handler.h"
#include "ui/views/view_tracker.h"
#include "ui/views/widget/widget_delegate.h"
#include "ui/views/widget/widget_observer.h"
#include "ui/views/window/client_view.h"
#include "ui/events/event.h"
#include "custom_browser/ui/mojom/ui_view_controller.mojom.h"
#include "custom_browser/ui/view/native_ui_container_view.h"
#include "custom_browser/ui/view/window_hit_test.h"
#include "custom_browser/ui/prefs/custom_browser_ui_prefs.h"
#if BUILDFLAG(IS_CHROMEOS)
#include "ui/compositor/compositor_metrics_tracker.h"
#endif

// NOTE: For more information about the objects and files in this directory,
// view: http://dev.chromium.org/developers/design-documents/browser-window

class AccessibilityFocusHighlight;
class BookmarkBarController;
class BookmarkBarView;
class Browser;
class BrowserWindowInterface;
class CustomBrowserUiPrefs;
class ContentsContainerView;
class ContentsLayoutManager;
struct DropData;
class ExclusiveAccessBubbleViews;
class FullscreenControlHost;
class FullscreenController;
class InfoBarContainerView;
class LocationBarView;
class MultiContentsView;
class ScrimView;
class SidePanel;
class TabDragDelegate;
class TabSearchBubbleHost;
class TabStrip;
class TabStripRegionView;
class ToolbarButtonProvider;
class ToolbarView;
class TopContainerLoadingBar;
class TopContainerView;
class TopControlsSlideController;
class TopControlsSlideControllerTest;
class WebAppFrameToolbarView;
class WebUITabStripContainerView;
class CustomBrowserWebUIView;
class MouseEventHandler;

namespace aura {
class Window;
}  // namespace aura

namespace gfx {
class AnimationRunner;
}  // namespace gfx

namespace ui {
class NativeTheme;
}  // namespace ui

namespace version_info {
enum class Channel;
}

namespace views {
class ExternalFocusTracker;
class View;
class WebView;
}  // namespace views

namespace webapps {
enum class InstallableWebAppCheckResult;
struct WebAppBannerData;
}  // namespace webapps

namespace enterprise_watermark {
class WatermarkView;
}

namespace custom_browser {
class CustomBrowserViewLayout;
}  // namespace custom_browser

struct CustomBrowserViews {
  CustomBrowserViews();
  CustomBrowserViews(CustomBrowserViews&&) noexcept;
  CustomBrowserViews& operator=(CustomBrowserViews&&) noexcept;
  ~CustomBrowserViews();
  // LINT.IfChange(CustomBrowserViews)
  raw_ptr<views::View> browser_view = nullptr;
  raw_ptr<CustomBrowserWebUIView> custom_browser_webui_view = nullptr;
  raw_ptr<NativeUIContainerView> native_ui_container = nullptr;
};

// CustomBrowserView implements the BrowserWindow interface for custom browser UI.
// It handles custom browser window behaviors and appearance.
class CustomBrowserView : public BrowserView,
                          public CustomBrowserUiPrefs::Observer,
                          public FullscreenObserver {

 public:
  explicit CustomBrowserView(Browser* browser);
  CustomBrowserView(const CustomBrowserView&) = delete;
  CustomBrowserView& operator=(const CustomBrowserView&) = delete;
  ~CustomBrowserView() override;

  using MouseMovePositionArea = custom_browser::mojom::MouseMovePositionArea;

  using PositionAreaInfo = custom_browser::mojom::PositionAreaInfo;

  enum NativeChildType {
    kActiveWebContentsChild = 0,
    kBackgroundWebContentsChild,
    kOtherChild
  };

  bool IsChromiumLayout() const;
  bool IsChromiumUI() const;
  static CustomBrowserView* FromWebContents(content::WebContents* web_contents);
  static CustomBrowserViews& FromBrowserView(views::View* browser_view);

  CustomBrowserViews& views();
  const CustomBrowserViews& views() const;
  void OnUIModeChanged(custom_browser::mojom::UIMode ui_mode) override;
  void OnPrefUIModeChanged(custom_browser::mojom::UIMode ui_mode) override;
  // ui:views::View overrides:
  void AddedToWidget() override;
  void RemovedFromWidget() override;

  bool ShouldDescendIntoChildForEventHandling(
      gfx::NativeView child,
      const gfx::Point& location) override;

  int NonClientHitTest(const gfx::Point& point) override;
  // void Layout(PassKey) override;

  // TabStripModelObserver: both restore the saved Chromium hierarchy before tab
  // detaches reach MaybeShowBookmarkBar(), which DCHECKs on our reparented
  // bookmark bar: WillCloseAllTabs on close paths, TabStripEmpty on a drag-out.
  // Both are idempotent.
  void WillCloseAllTabs(TabStripModel* tab_strip_model) override;
  void TabStripEmpty() override;
  // A single-web window holds one tab: a tab inserted by any other path (a
  // profile-level open, a restored tab) moves to a normal window (F-29).
  void OnTabStripModelChanged(
      TabStripModel* tab_strip_model,
      const TabStripModelChange& change,
      const TabStripSelectionChange& selection) override;

  // BrowserWindow:
  // In the custom layouts a tab activated while <main-contents-view> is
  // hidden (kNexus: no guest shows it) never gets a size from its hidden
  // NativeViewHost; size it here. See
  // CustomBrowserViewLayout::SizeUndisplayedTabs().
  void OnActiveTabChanged(content::WebContents* old_contents,
                          content::WebContents* new_contents,
                          int index,
                          int reason) override;

  // FullscreenObserver:
  void OnFullscreenStateChanged() override;

  // views::WidgetDelegate:
  // In kNexus mode the native window title (taskbar / alt-tab / a11y) is
  // "{session title} - {product name}", falling back to the bare product name
  // until a session title is published. Fed by the guest's
  // `ui.setActiveSessionTitle` bridge action. Deliberately not
  // Browser::SetWindowUserTitle: user_title_ round-trips through
  // SessionService (stale titles on restore) and collides with Chromium's
  // "Name window…" feature.
  std::u16string GetWindowTitle() const override;
  std::u16string GetAccessibleWindowTitle() const override;
  // ui::AcceleratorTarget: a single-web window swallows the shortcuts of
  // commands outside IsCommandAllowedInSingleWebWindow().
  bool AcceleratorPressed(const ui::Accelerator& accelerator) override;
  // views::WidgetDelegate: the same allowlist for Windows system-menu and
  // app commands (WM_SYSCOMMAND / WM_APPCOMMAND), which bypass accelerators.
  bool ExecuteWindowsCommand(int command_id) override;

  // Stores the active Nexus session's display title (already sanitized by
  // NexusUiBridgeAdapter) and refreshes the title bar. Empty clears back to
  // the bare product name. No-op when unchanged.
  void SetNexusSessionTitle(const std::u16string& title);

  // True when this window runs (or is booting into) the Nexus UI — includes
  // the pending phase between the kNexus decision in
  // InitializeCustomBrowser() and the mode landing via the WebUI layout
  // load. The pending-aware form is what window routing and startup-prompt
  // suppression must use (see window_util::IsNexusBrowser); decisions made
  // during boot would otherwise misclassify this window as normal.
  bool IsNexusUIMode() const;

  // True for a window created by the `webWindow` bridge capability (F-29): it
  // is pinned to kSingleWeb for its whole life, from InitializeCustomBrowser()
  // on, before the mode lands.
  bool IsSingleWebWindow() const { return single_web_window_; }

  CustomBrowserUiPrefs* ui_prefs() const { return ui_prefs_.get(); }
  NativeUIContainerView* native_ui_container() { return native_ui_container_.get(); }
  void SetActiveTabSearchButton(views::View* button);
  custom_browser::CustomBrowserViewLayout* GetCustomBrowserViewLayout() const;
  void LayoutCustomBrowserViewNow();
  bool StartMouseCapture(const gfx::Rect& bounds,
                         int capture_id,
                         base::TimeDelta delay,
                         base::OnceCallback<void(bool)> completion_callback);
  void OnPreHandleRootViewMouseEvent(gfx::Point location_in_this, ui::MouseEvent* event);
  // custom-browser: was a BrowserListObserver override in the 146 baseline;
  // Chromium 149 removed BrowserListObserver in favor of per-browser
  // RegisterBrowserDidClose() callbacks.
  void OnBrowserRemoved(BrowserWindowInterface* browser);
private:
  friend class MouseEventHandler;

  void ObserveFullscreenController();
  void InitializeCustomBrowser();
  void UpdateVisibilityAndOrder();
  // Restores the saved Chromium view hierarchy (bookmark/loading bars, etc.)
  // back under their upstream parents during window teardown. Idempotent.
  // See WillCloseAllTabs()/TabStripEmpty() for why this must run before tab
  // detach on close.
  void RestoreChromiumHierarchyForTeardown();
  static std::optional<gfx::Rect> GetTabSearchAnchorRectOverride(
      const views::View* anchor_view,
      const gfx::Rect& default_rect);
  void PerformCustomLayout();
  void LayoutCustomBrowserViewImpl();

  void EnsureMouseEventFilter();
  void ResetMouseEventFilter();

  // Which resize-gutter edge `point_in_this` (this view's coordinate space)
  // falls on, or kNone when the window can't be resized (maximized,
  // fullscreen, CanResize() false) or the point is off the gutter. Shared by
  // NonClientHitTest (drives WM_NCHITTEST and thus WM_SETCURSOR) and
  // ShouldDescendIntoChildForEventHandling (keeps aura targeting from handing
  // gutter events to a web contents, whose window would recompute the cursor
  // as an arrow) — the two must agree or the resize cursor flickers.
  custom_browser::ResizeEdge ResizeEdgeAtLocalPoint(
      const gfx::Point& point_in_this) const;

  void UpdatePositionArea(const gfx::Point& location);
  NativeChildType GetNativeChildType(gfx::NativeView child);
  void OnMouseCaptureFinished(bool success);
  void RunCaptureCallback(bool success);

  std::unique_ptr<CustomBrowserWebUIView> custom_browser_webui_view_;
  std::unique_ptr<NativeUIContainerView> native_ui_container_;
  std::unique_ptr<CustomBrowserUiPrefs> ui_prefs_;

  // Active session title published by the guest via
  // `ui.setActiveSessionTitle`; composed into GetWindowTitle() while in
  // kNexus mode. Never persisted.
  std::u16string nexus_session_title_;

  // True from the moment InitializeCustomBrowser() decides this window opens
  // in kNexus until the mode actually lands (OnUIModeChanged). current_mode()
  // stays at its pre-init default until the WebUI's BuildInitUrl() loads the
  // layout params — without this flag the window's first frames would title
  // as a normal browser ("about:blank - <app>") instead of the product name.
  bool nexus_mode_pending_ = false;

  // Set once in InitializeCustomBrowser() from the pending single-web marker.
  bool single_web_window_ = false;
  void MoveTabsToNormalWindow(std::vector<tabs::TabHandle> tabs);

  bool custom_layout_allowed_ = false;
  bool is_custom_initialized_ = false;
  PositionAreaInfo last_mouse_move_position_area_;
  CustomBrowserViews views_;
  // TODO: Add implementation details
  void StopMouseCapture(bool success);
  std::optional<int> active_capture_id_;
  base::OnceCallback<void(bool)> capture_finished_callback_;
  base::ScopedObservation<FullscreenController, FullscreenObserver>
      fullscreen_observation_{this};
  base::CallbackListSubscription browser_did_close_subscription_;
  std::unique_ptr<MouseEventHandler> mouse_event_filter_;
  views::ViewTracker active_tab_search_button_tracker_;
  base::WeakPtrFactory<CustomBrowserView> weak_ptr_factory_{this};
};

#endif  // CUSTOM_BROWSER_UI_CUSTOM_BROWSER_VIEW_H_
