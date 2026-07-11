#include "custom_browser/ui/view/custom_browser_view.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/supports_user_data.h"
#include "base/task/sequenced_task_runner.h"
#include "ui/events/event.h"            // ui::MouseEvent
#include "ui/events/types/event_type.h"            // ui::EventType
#include "base/logging.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_features.h"
#include "chrome/browser/ui/exclusive_access/exclusive_access_manager.h"
#include "chrome/browser/ui/views/frame/contents_container_view.h"
#include "chrome/browser/ui/views/frame/multi_contents_view.h"
#include "chrome/browser/ui/views/frame/top_container_view.h"
#include "custom_browser/ui/view/mouse_event_handler.h"
#include "chrome/browser/ui/views/tab_search_bubble_host.h"
#include "custom_browser/chromium_hooks/hook_dispatcher.h"
#include "custom_browser/chromium_hooks/hook_registrar.h"
#include "custom_browser/ui/view/custom_browser_view_layout.h"
#include "custom_browser/ui/view/custom_browser_window_util.h"
#include "custom_browser/ui/web_ui/custom_browser_webui_view.h"
#include "custom_browser/ui/web_ui/custom_browser_webui.h"

#include "content/browser/web_contents/web_contents_impl.h"
#include "chrome/browser/ui/sad_tab_helper.h"
#include "ui/base/hit_test.h"
#include "ui/gfx/geometry/point.h"
#include "ui/aura/window.h"
#include "chrome/browser/ui/views/side_panel/side_panel.h"
#include "chrome/browser/ui/views/infobars/infobar_container_view.h"
#include "chrome/browser/ui/views/bookmarks/bookmark_bar_view.h"
#include "components/infobars/content/content_infobar_manager.h"
#include "ui/views/widget/widget.h"
#include "ui/views/view.h"
#include "ui/views/controls/webview/webview.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>
#include <type_traits>
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "ui/views/win/hwnd_util.h"  // views::HWNDForWidget
#include "chrome/browser/ui/views/frame/browser_caption_button_container_win.h"
#endif
//#if BUILDFLAG(USE_AURA)
#include "ui/aura/window.h"
#include "ui/aura/client/window_types.h"  // aura::client::GetWindowType
//#endif

#include "base/feature_list.h"


namespace {

// Command-line switch: when present, the first window of a profile opens in a
// browser mode (pref_mode()) instead of the default kNexus startup window.
constexpr char kBrowserModeSwitch[] = "browser-mode";
constexpr char kStartupDefaultConsumedKey[] =
    "custom_browser_startup_default_consumed";

class StartupDefaultConsumedMarker : public base::SupportsUserData::Data {};

// Returns true exactly once per profile, for the first normal window created.
// Backed by transient Profile user-data so it is lifetime-safe and never
// persisted.
bool ConsumeStartupDefault(Profile* profile) {
  if (!profile || profile->GetUserData(kStartupDefaultConsumedKey)) {
    return false;
  }
  profile->SetUserData(kStartupDefaultConsumedKey,
                       std::make_unique<StartupDefaultConsumedMarker>());
  return true;
}

}  // namespace

CustomBrowserViews::CustomBrowserViews() = default;
CustomBrowserViews::CustomBrowserViews(
    CustomBrowserViews&&) noexcept = default;
CustomBrowserViews& CustomBrowserViews::operator=(
    CustomBrowserViews&&) noexcept = default;
CustomBrowserViews::~CustomBrowserViews() noexcept = default;


CustomBrowserView::CustomBrowserView(Browser* browser)
    : BrowserView(browser) {
  DCHECK(browser_);
  browser_did_close_subscription_ =
      browser->RegisterBrowserDidClose(base::BindRepeating(
          &CustomBrowserView::OnBrowserRemoved, base::Unretained(this)));
  views_.browser_view = this;
}

CustomBrowserView::~CustomBrowserView() {
  StopMouseCapture(false);
  fullscreen_observation_.Reset();
  // Tear members down in a safe, explicit order — they hold raw references to
  // each other / to ui_prefs_, so the implicit reverse-declaration order is
  // NOT safe:
  //   * native_ui_container_ was Init()'d with custom_browser_webui_view_ and
  //     references ui_prefs_ during destruction to detach.
  //   * custom_browser_webui_view_'s WebUI owns a UIViewHandler that holds a
  //     raw ui_prefs_ pointer and calls ui_prefs_->RemoveObserver(this) in its
  //     destructor (native_view_handler.cc). That raw pointer is NOT nulled
  //     when the unique_ptr below resets, so destroying the WebUI view AFTER
  //     ui_prefs_ is a use-after-free (libc++ hardening aborts in
  //     ObserverList::RemoveObserver).
  // Order: native_ui_container_ (references both) -> webui view (references
  // ui_prefs_) -> ui_prefs_ last.
  native_ui_container_.reset();
  custom_browser_webui_view_.reset();
  ui_prefs_.reset();
}

CustomBrowserViews& CustomBrowserView::views() {
  return views_;
}

const CustomBrowserViews& CustomBrowserView::views() const {
  return views_;
}

custom_browser::CustomBrowserViewLayout*
CustomBrowserView::GetCustomBrowserViewLayout() const {
  auto* layout = GetBrowserViewLayout();
  return static_cast<custom_browser::CustomBrowserViewLayout*>(layout);
}

// static
CustomBrowserViews& CustomBrowserView::FromBrowserView(views::View* browser_view) {
  auto* custom_browser_view = static_cast<CustomBrowserView*>(browser_view);
  return custom_browser_view->views();
}

bool CustomBrowserView::IsChromiumLayout() const {
  if (!GetIsNormalType() || IsChromiumUI()) {
    return true;
  }
  // For normal browsers in Chromium UI mode, we use Chromium layout only
  // when not in fullscreen.
  return IsFullscreen();
}

bool CustomBrowserView::IsChromiumUI() const {
  auto ui_mode = ui_prefs_ ? ui_prefs_->current_mode() : UIMode::kStandard;
  return (ui_mode == UIMode::kStandard);
}

void CustomBrowserView::AddedToWidget() {
    BrowserView::AddedToWidget();
    ObserveFullscreenController();
    if (!is_custom_initialized_) {
        // Perform any custom initialization here.
        is_custom_initialized_ = true;
        if (browser_->is_type_normal()) {
          InitializeCustomBrowser();
        }
    }
}

void CustomBrowserView::RemovedFromWidget() {
  StopMouseCapture(false);
  fullscreen_observation_.Reset();
  BrowserView::RemovedFromWidget();
}

void CustomBrowserView::OnUIModeChanged(custom_browser::mojom::UIMode ui_mode) {
  UpdateVisibilityAndOrder();
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<CustomBrowserView> view) {
            if (!view) {
              return;
            }
            view->InvalidateLayout();
            if (auto* widget = view->GetWidget()) {
              widget->LayoutRootViewIfNecessary();
            }
          },
          weak_ptr_factory_.GetWeakPtr()));
}

void CustomBrowserView::OnPrefUIModeChanged(custom_browser::mojom::UIMode ui_mode) {
  //  if (ui_mode != custom_browser::mojom::UIMode::kStandard) {
  //     custom_browser_webui_view_->SetVisible(true);
  //  }
}

void CustomBrowserView::SetActiveTabSearchButton(views::View* button) {
  if (button) {
    active_tab_search_button_tracker_.SetView(button);
  } else {
    active_tab_search_button_tracker_.SetView(nullptr);
  }
}

// static
std::optional<gfx::Rect> CustomBrowserView::GetTabSearchAnchorRectOverride(
    const views::View* anchor_view,
    const gfx::Rect& default_rect) {
  if (!anchor_view || !anchor_view->GetWidget()) {
    return std::nullopt;
  }
  auto* browser_view = BrowserView::GetBrowserViewForNativeWindow(
      anchor_view->GetWidget()->GetNativeWindow());
  if (!browser_view || !browser_view->GetIsNormalType()) {
    return std::nullopt;
  }
  auto* custom_view = static_cast<CustomBrowserView*>(browser_view);
  if (custom_view->IsChromiumUI()) {
    return std::nullopt;
  }
  // Only redirect tab search bubbles.
  if (!custom_view->tab_search_bubble_host_ ||
      anchor_view != custom_view->tab_search_bubble_host_->button()) {
    return std::nullopt;
  }
  auto* tracked = custom_view->active_tab_search_button_tracker_.view();
  if (!tracked || !tracked->GetVisible() || !tracked->GetWidget()) {
    return std::nullopt;
  }
  return tracked->GetBoundsInScreen();
}

void CustomBrowserView::InitializeCustomBrowser() {
    ui_prefs_ = std::make_unique<CustomBrowserUiPrefs>(browser()->profile()->GetPrefs());
    ui_prefs_->AddObserver(this);

    // Decide whether this window should open directly in kNexus, before the
    // WebUI view triggers BuildInitUrl() below.
    Profile* profile = browser()->profile();
    bool want_nexus =
        custom_browser::window_util::ConsumePendingNexusWindow(profile);
    if (!want_nexus && ConsumeStartupDefault(profile)) {
      // Normal startup defaults the first window to kNexus unless launched with
      // the browser-mode switch.
      want_nexus = !base::CommandLine::ForCurrentProcess()->HasSwitch(
          kBrowserModeSwitch);
    }
    if (want_nexus && !custom_browser::window_util::FindNexusWindow(profile)) {
      ui_prefs_->set_initial_mode_override(UIMode::kNexus);
    }

    native_ui_container_ = std::make_unique<NativeUIContainerView>(this);
    views_.native_ui_container = native_ui_container_.get();
    native_ui_container_->SaveChromiumHierarchy();

    AddChildView(native_ui_container_.get());

    custom_browser_webui_view_ = std::make_unique<CustomBrowserWebUIView>(this);
    views_.custom_browser_webui_view = custom_browser_webui_view_.get();
    AddChildView(custom_browser_webui_view_.get());

    native_ui_container_->Init(custom_browser_webui_view_.get());

    ReorderChildView(custom_browser_webui_view_.get(), 0);
    ReorderChildView(native_ui_container_.get(), 1);
    custom_browser::GetHookRegistrar()->SetBubbleAnchorRectOverride(
        base::BindRepeating(
            &CustomBrowserView::GetTabSearchAnchorRectOverride));
    UpdateVisibilityAndOrder();
}

void CustomBrowserView::UpdateVisibilityAndOrder() {
  if (IsChromiumUI()) {
    SetActiveTabSearchButton(nullptr);
    native_ui_container_->RestoreChromiumHierarchy();
  }
  if (IsChromiumLayout()) {
    ResetMouseEventFilter();
  } else {
    EnsureMouseEventFilter();
  }
  // GetCustomBrowserViewLayout()->OnUIStateChanged(IsChromiumUI(), IsChromiumLayout());
}

void CustomBrowserView::OnFullscreenStateChanged() {
  UpdateVisibilityAndOrder();
}

void CustomBrowserView::UpdatePositionArea(const gfx::Point& location) {
  PositionAreaInfo info;
  info.area = MouseMovePositionArea::kUnknownArea;
  info.position = location;
  if (WidgetOwnedByAnchorContainsPoint(location)) {
    info.area = MouseMovePositionArea::kWidgetOwnedArea;
  } else if (contents_container()->bounds().Contains(location) && contents_container()->GetVisible()) {
    auto* contents_container_view = GetActiveContentsContainerView();
    views::WebView* devtools_view =
        contents_container_view ? contents_container_view->devtools_web_view()
                                : nullptr;
    if (devtools_view && devtools_view->GetVisible() &&
        devtools_view->bounds().Contains(location)) {
      info.area = MouseMovePositionArea::kDevToolsArea;
    } else {
      info.area = MouseMovePositionArea::kActiveWebContentsArea;
    }
    // custom-browser: Chromium 149 unified the former contents-height and
    // toolbar-height side panels into a single BrowserView::side_panel().
  } else if (auto* unified_side_panel = side_panel();
             unified_side_panel && unified_side_panel->GetVisible() &&
             unified_side_panel->bounds().Contains(location)) {
    info.area = MouseMovePositionArea::kSidePanelArea;
  } else if (infobar_container() && infobar_container()->GetVisible() &&
             infobar_container()->bounds().Contains(location)) {
    // Map InfoBar area into native view area for WebUI consumers.
    info.area = MouseMovePositionArea::kInfoBarArea;
  } else if (auto* view = native_ui_container_->FindVisibleView(location)) {
    info.view_class_name = view->GetClassName();
    info.area = MouseMovePositionArea::kVisibleNativeViewArea;
  #if BUILDFLAG(IS_WIN)
    if (view->GetClassName() == BrowserCaptionButtonContainer::kViewClassName) {
      info.area = MouseMovePositionArea::kWindowCaptionArea;
    }
  #endif
  } else if (custom_browser_webui_view_->bounds().Contains(location)) {
    const gfx::Point web_view_point =
        custom_browser_webui_view_->ConvertPointToWebUITarget(this, location);
    if (custom_browser_webui_view_->IsInWebUIDragRegion(web_view_point)) {
      info.area = MouseMovePositionArea::kBackgroundWebContentsDragArea;
    } else {
      info.area = MouseMovePositionArea::kBackgroundWebContentsArea;
    }
  } else if (!bounds().Contains(location)) {
    info.area = MouseMovePositionArea::kOtherBrowserArea;
  }
  last_mouse_move_position_area_ = info;
}

CustomBrowserView::NativeChildType CustomBrowserView::GetNativeChildType(gfx::NativeView child) {
  if (auto* wc = GetActiveWebContents()) {
    if (child->Contains(wc->GetNativeView())) {
      return kActiveWebContentsChild;
    }
  }
  if (auto* wc = custom_browser_webui_view_->GetWebContents()) {
    if (child->Contains(wc->GetNativeView())) {
      return kBackgroundWebContentsChild;
    }
  }
  return kOtherChild;
}

void CustomBrowserView::ObserveFullscreenController() {
  if (fullscreen_observation_.IsObserving() || !browser()) {
    return;
  }

  BrowserWindowFeatures* features = browser()->browser_window_features();
  if (!features) {
    return;
  }

  auto* exclusive_access_manager = features->exclusive_access_manager();
  if (!exclusive_access_manager) {
    return;
  }

  FullscreenController* fullscreen_controller =
      exclusive_access_manager->fullscreen_controller();
  if (!fullscreen_controller) {
    return;
  }

  fullscreen_observation_.Observe(fullscreen_controller);
}

bool CustomBrowserView::ShouldDescendIntoChildForEventHandling(
    gfx::NativeView child,
    const gfx::Point& location) {
  if (IsChromiumLayout()) {
    return BrowserView::ShouldDescendIntoChildForEventHandling(child, location);
  }
  auto child_type = GetNativeChildType(child);
  if (child_type == kBackgroundWebContentsChild) {
    if (last_mouse_move_position_area_.area != MouseMovePositionArea::kBackgroundWebContentsArea) {
      return false;
    }
  }
 return true;
}

int CustomBrowserView::NonClientHitTest(const gfx::Point& point) {
  if (IsChromiumLayout()) {
    return BrowserView::NonClientHitTest(point);
  }
  auto position_area = last_mouse_move_position_area_.area;
  if (position_area == MouseMovePositionArea::kBackgroundWebContentsDragArea) {
    return HTCAPTION;
  } else if (position_area == MouseMovePositionArea::kWindowCaptionArea) {
    // We return HTNOWHERE as this is a signal to our containing
    // NonClientView that it should figure out what the correct hit-test
    // code is given the mouse position...
    return HTNOWHERE;
  }
  return HTCLIENT;
}

bool CustomBrowserView::StartMouseCapture(
    const gfx::Rect& bounds,
    int capture_id,
    base::TimeDelta delay,
    base::OnceCallback<void(bool)> completion_callback) {
  if (completion_callback.is_null()) {
    return false;
  }

  if (active_capture_id_.has_value()) {
    StopMouseCapture(/*success=*/false);
  }

  EnsureMouseEventFilter();
  if (!mouse_event_filter_) {
    std::move(completion_callback).Run(false);
    return false;
  }

  active_capture_id_ = capture_id;
  capture_finished_callback_ = std::move(completion_callback);

  const bool started = mouse_event_filter_->StartCapture(
      bounds, delay,
      base::BindOnce(&CustomBrowserView::OnMouseCaptureFinished,
                     weak_ptr_factory_.GetWeakPtr()));

  if (!started) {
    RunCaptureCallback(/*success=*/false);
  }

  return started;
}

void CustomBrowserView::StopMouseCapture(bool success) {
  if (mouse_event_filter_) {
    mouse_event_filter_->StopCapture(success);
    return;
  }

  if (active_capture_id_.has_value()) {
    RunCaptureCallback(success);
  }
}

void CustomBrowserView::EnsureMouseEventFilter() {
  if (mouse_event_filter_) {
    return;
  }

  aura::Window* root_window = nullptr;
  if (auto* widget = GetWidget()) {
    if (auto* native_window = widget->GetNativeWindow()) {
      root_window = native_window->GetRootWindow();
    }
  }

  if (!root_window) {
    return;
  }

  mouse_event_filter_ =
      std::make_unique<MouseEventHandler>(this, root_window);
}

void CustomBrowserView::ResetMouseEventFilter() {
  if (!mouse_event_filter_) {
    return;
  }
  mouse_event_filter_->StopCapture(/*success=*/false);
  mouse_event_filter_.reset();
}

void CustomBrowserView::OnMouseCaptureFinished(bool success) {
  RunCaptureCallback(success);
}

void CustomBrowserView::RunCaptureCallback(bool success) {
  if (!active_capture_id_.has_value()) {
    return;
  }

  active_capture_id_.reset();

  if (capture_finished_callback_) {
    auto cb = std::move(capture_finished_callback_);
    capture_finished_callback_.Reset();
    std::move(cb).Run(success);
  }
}

void CustomBrowserView::OnPreHandleRootViewMouseEvent(gfx::Point location_in_this, ui::MouseEvent* event) {
  DCHECK(event);
  UpdatePositionArea(location_in_this);
  if ((last_mouse_move_position_area_.area == MouseMovePositionArea::kWindowCaptionArea) &&
      (event->type() == ui::EventType::kMouseExited)) {
    event->StopPropagation();
    event->SetHandled();
  }
  // LOG(ERROR) << "Mouse event type=" << static_cast<int>(event->type())
  //            << " at " << location_in_this.ToString()
  //            << " area=" << static_cast<int>(last_mouse_move_position_area_.area);
}

void CustomBrowserView::OnBrowserRemoved(BrowserWindowInterface* browser) {
  // RegisterBrowserDidClose() only fires for this view's own browser, so the
  // previous BrowserList-wide identity filter is no longer required.
  if (ui_prefs_) {
    ui_prefs_->OnBrowserClosing();
  }
  RestoreChromiumHierarchyForTeardown();
}

void CustomBrowserView::WillCloseAllTabs(TabStripModel* tab_strip_model) {
  // custom-browser (149): restore the Chromium view hierarchy BEFORE any tab is
  // detached. Browser::OnWindowClosing() -> TabStripModel::CloseAllTabs() ->
  // CloseTabs() notifies WillCloseAllTabs() first, then drives the per-tab
  // detach notifications. Those detaches synchronously reach
  // BrowserView::OnTabDetached() -> ToolbarSizeChanged() ->
  // UpdateUIForContents() -> MaybeShowBookmarkBar(), which in 149 does
  // `top_container_->RemoveChildViewT(bookmark_bar_view_)` and DCHECKs because
  // the custom layout reparented the bookmark bar under native_ui_container_.
  // Restoring here (before the detaches) puts the bookmark/loading bars back
  // under top_container_ so that removal succeeds. RestoreViewHierarchy is
  // idempotent, so the later TabStripEmpty()/OnBrowserRemoved() restores remain
  // harmless no-ops.
  RestoreChromiumHierarchyForTeardown();
  BrowserView::WillCloseAllTabs(tab_strip_model);
}

void CustomBrowserView::TabStripEmpty() {
  // Second restore for the "strip emptied via tab drag-out" path, which does
  // not go through CloseAllTabs()/WillCloseAllTabs(). Idempotent — see
  // WillCloseAllTabs() above for the full rationale.
  RestoreChromiumHierarchyForTeardown();
  BrowserView::TabStripEmpty();
}

void CustomBrowserView::RestoreChromiumHierarchyForTeardown() {
  if (native_ui_container_) {
    native_ui_container_->RestoreChromiumHierarchy();
  }
  // The BookmarkBarView is created lazily by BrowserView::MaybeShowBookmarkBar
  // — AFTER NativeUIContainerView::SaveChromiumHierarchy() snapshotted the view
  // tree at init — so RestoreChromiumHierarchy() above can never restore it.
  // BrowserView (149) believes it owns the bar under top_container_ (its
  // detached_bookmark_bar_view_ is null once attached) and on teardown does
  // `top_container_->RemoveChildViewT(bookmark_bar_view_)`. Because the custom
  // layout (CustomBrowserViewLayout::FixViewsParentForCustom) reparented the
  // bar under native_ui_container_, that removal DCHECKs on
  // `std::ranges::contains(top_container_->children(), bar)`. Move the bar back
  // under top_container_ here so the upstream removal stays valid. Only act
  // when the bar currently has a (non-top_container_) parent: a null parent
  // means it is the detached unique_ptr, in which case the remove branch does
  // not run. Mirrors FixViewsParentForCustom()'s AddChildViewRaw usage.
  if (BookmarkBarView* bar = bookmark_bar()) {
    TopContainerView* tc = top_container();
    if (tc && bar->parent() && bar->parent() != tc) {
      tc->AddChildViewRaw(bar);
    }
  }
}
