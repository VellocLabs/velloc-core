#include "custom_browser/ui/web_ui/custom_browser_webui_view.h"

#include <utility>

#include "base/feature_list.h"
#include "base/functional/callback.h"
#include "base/logging.h"
#include "base/functional/callback_helpers.h"
#include "base/no_destructor.h"
#include "base/process/process_handle.h"
#include "base/scoped_observation.h"
#include "base/synchronization/atomic_flag.h"
#include "base/task/single_thread_task_runner.h"
#include "base/timer/timer.h"
#include "base/trace_event/trace_event.h"
#include "chrome/browser/extensions/extension_tab_util.h"
#include "chrome/browser/lifetime/application_lifetime.h"
#include "base/supports_user_data.h"
#include "chrome/browser/lifetime/browser_shutdown.h"
#include "content/public/browser/web_contents.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/task_manager/web_contents_tags.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/ui_features.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/immersive_mode_controller.h"
#include "chrome/browser/ui/views/tabs/groups/tab_group_editor_bubble_view.h"
#include "custom_browser/ui/web_ui/tab_strip_ui_util.h"
#include "chrome/common/webui_url_constants.h"
#include "content/public/browser/context_menu_params.h"
#include "content/public/browser/eye_dropper_listener.h"
#include "content/public/browser/file_select_listener.h"
#include "content/public/browser/media_stream_request.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_ui.h"
#include "content/public/common/drop_data.h"
#include "content/public/common/referrer.h"
#include "content/public/common/window_container_type.mojom-shared.h"
#include "custom_browser/browser/nexus/panel/nexus_background_panel_host.h"
#include "custom_browser/ui/prefs/custom_browser_ui_prefs.h"
#include "custom_browser/ui/view/custom_browser_view.h"
#include "custom_browser/ui/web_ui/custom_browser_constants.h"
#include "custom_browser/ui/web_ui/custom_browser_webui.h"
#include "custom_browser/ui/web_ui/nexus_file_drop_policy.h"
#include "third_party/blink/public/common/renderer_preferences/renderer_preferences.h"
#include "ui/aura/window_delegate.h"
#include "ui/base/clipboard/clipboard_format_type.h"
#include "ui/base/clipboard/custom_data_helper.h"
#include "ui/base/dragdrop/os_exchange_data.h"
#include "ui/base/hit_test.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/mojom/menu_source_type.mojom.h"
#include "ui/base/window_open_disposition.h"
#include "ui/color/color_provider.h"
#include "ui/events/event.h"
#include "ui/events/event_utils.h"
#include "ui/events/gesture_event_details.h"
#include "ui/views/controls/menu/menu_runner.h"
#include "ui/views/controls/webview/webview.h"
#include "ui/views/layout/fill_layout.h"
#include "ui/views/view_utils.h"
#include "ui/views/widget/widget.h"
#include "url/gurl.h"

namespace {

// Window-drag state stashed on a parked WebContents (base::SupportsUserData)
// so the next window restores it: the page's `-webkit-app-region` regions
// are only re-reported by blink when they CHANGE, and the guest topbar only
// re-measures on resize, so a re-attached page at the same size would
// otherwise leave the new view with no regions — and IsInWebUIDragRegion's
// "not ready" fallback treats the whole page as caption.
constexpr char kParkedDragStateKey[] = "custom_browser_parked_drag_state";
struct ParkedDragState : public base::SupportsUserData::Data {
  SkRegion draggable_region;
  bool is_draggable_ready = false;
  SkRegion guest_drag_region;
  bool guest_drag_ready = false;
};

CustomBrowserWebUI* GetBrowserWebUI(content::WebContents* web_contents) {
  content::WebUI* const webui = web_contents->GetWebUI();
  return webui && webui->GetController()
             ? webui->GetController()->template GetAs<CustomBrowserWebUI>()
             : nullptr;
}
}  // namespace

CustomBrowserWebUIView::CustomBrowserWebUIView(
    CustomBrowserView* custom_browser_view)
    : custom_browser_view_(custom_browser_view),
      web_view_(AddChildView(std::make_unique<views::WebView>(
          custom_browser_view_->GetProfile()))) {
  TRACE_EVENT0("ui", "CustomBrowserWebUIView.Init");

  SetVisible(false);
  view_observations_.AddObservation(custom_browser_view_.get());

  // A kNexus window first asks the profile's background-panel host for a
  // parked contents (the page a previous kNexus window left running when it
  // closed); otherwise create a fresh one. `web_view_` never owns it.
  Profile* profile = custom_browser_view_->GetProfile();
  bool reattached = false;
  if (custom_browser_view_->IsNexusUIMode()) {
    if (auto* host =
            nexus::NexusBackgroundPanelHostFactory::GetForProfile(profile)) {
      owned_contents_ = host->Take();
      reattached = owned_contents_ != nullptr;
    }
  }
  if (!owned_contents_) {
    owned_contents_ =
        content::WebContents::Create(content::WebContents::CreateParams(profile));
    // TODO(crbug.com/40651211) WebContents are initially assumed to be visible
    // by default unless explicitly hidden. The WebContents need to be set to
    // hidden so that the visibility state of the document in JavaScript is
    // correctly initially set to 'hidden', and the 'visibilitychange' events
    // correctly get fired.
    owned_contents_->WasHidden();
  }
  web_view_->SetWebContents(owned_contents_.get());

  // NOTE: Do NOT enable `set_allow_accelerators(true)` here.
  //
  // This view was adapted from Chromium's WebUITabStripContainerView, which
  // enables it because a tab-strip toolbar has no editable fields and wants
  // browser accelerators (and arrow-key focus traversal) handled by the
  // views FocusManager before the page.
  //
  // The Nexus panel is the opposite: it's a full interactive UI with text
  // inputs. With `allow_accelerators_ = true`,
  // WebView::SkipDefaultKeyEventProcessing only returns true for Tab, so
  // EVERY other key — including arrow keys — is routed to the FocusManager
  // before the web content. The FocusManager then consumes arrow keydowns
  // for focus traversal, so they never reach the page and the text caret
  // can't move (focus jumps to a neighboring control instead).
  //
  // Leaving it at the default (false) makes SkipDefaultKeyEventProcessing
  // return IsWebContentsAlive() — the page receives keys first (caret moves
  // normally), and any key the page does NOT consume still bubbles back and
  // is treated as a browser accelerator. That is the standard tab-contents
  // model and is what an interactive panel needs.

  SetLayoutManager(std::make_unique<views::FillLayout>());
  InitializeWebView(reattached);
  reattached_pending_show_ = reattached;

  SetContainerTargetVisibility(true);
  // SetPaneFocusAndFocusDefault();
}

CustomBrowserWebUIView::~CustomBrowserWebUIView() {
  if (ShouldParkForBackground()) {
    ParkForBackground();
    return;
  }
  DeinitializeWebView();
  // `web_view_` (a child view, destroyed after this body) holds a raw
  // pointer to the contents: unhook it before the contents dies.
  web_view_->SetWebContents(nullptr);
  owned_contents_.reset();
}

bool CustomBrowserWebUIView::ShouldParkForBackground() const {
  if (!owned_contents_ || owned_contents_->IsCrashed()) {
    return false;
  }
  if (!custom_browser_view_ || !custom_browser_view_->IsNexusUIMode()) {
    return false;  // Only the kNexus window's page is worth keeping.
  }
  if (browser_shutdown::HasShutdownStarted()) {
    return false;  // The process is going away; nothing to reopen into.
  }
  if (!nexus::NexusBackgroundPanelHost::IsBackgroundPanelEnabled()) {
    return false;  // The System toggle is off: close means close.
  }
  return nexus::NexusBackgroundPanelHostFactory::GetForProfile(
             custom_browser_view_->GetProfile()) != nullptr;
}

void CustomBrowserWebUIView::ParkForBackground() {
  auto* host = nexus::NexusBackgroundPanelHostFactory::GetForProfile(
      custom_browser_view_->GetProfile());
  DCHECK(host);
  content::WebContentsObserver::Observe(nullptr);
  if (auto* custom_browser_webui = GetBrowserWebUI(owned_contents_.get())) {
    // Handlers survive, paused, for the next window (AttachToView).
    custom_browser_webui->DetachFromView();
  }
  auto drag_state = std::make_unique<ParkedDragState>();
  drag_state->draggable_region = draggable_region_;
  drag_state->is_draggable_ready = is_draggable_ready_;
  drag_state->guest_drag_region = guest_drag_region_;
  drag_state->guest_drag_ready = guest_drag_ready_;
  owned_contents_->SetUserData(kParkedDragStateKey, std::move(drag_state));
  owned_contents_->SetDelegate(nullptr);
  web_view_->SetWebContents(nullptr);
  // Detaching from the WebView already reports hidden; make it explicit so
  // the page sees 'hidden' while parked (glic does the mirror-image
  // UpdateWebContentsVisibility(VISIBLE) on show).
  owned_contents_->WasHidden();
  host->Park(std::move(owned_contents_));
}

// static
void CustomBrowserWebUIView::GetDropFormatsForView(
    int* formats,
    std::set<ui::ClipboardFormatType>* format_types) {
  *formats |= ui::OSExchangeData::PICKLED_DATA;
  format_types->insert(ui::ClipboardFormatType::DataTransferCustomType());
}

// static
bool CustomBrowserWebUIView::IsDraggedTab(const ui::OSExchangeData& data) {
  std::optional<base::Pickle> pickle =
      data.GetPickledData(ui::ClipboardFormatType::DataTransferCustomType());
  if (!pickle.has_value()) {
    return false;
  }

  if (std::optional<std::u16string> result =
          ui::ReadCustomDataForType(pickle.value(), kCustomWebUITabIdDataType);
      result && !result->empty()) {
    return true;
  }

  if (std::optional<std::u16string> result = ui::ReadCustomDataForType(
          pickle.value(), kCustomWebUITabGroupIdDataType);
      result && !result->empty()) {
    return true;
  }

  return false;
}

views::NativeViewHost* CustomBrowserWebUIView::GetNativeViewHost() {
  return web_view_->holder();
}

content::WebContents* CustomBrowserWebUIView::GetWebContents() {
  return owned_contents_.get();
}

// const ui::AcceleratorProvider*
// CustomBrowserWebUIView::GetAcceleratorProvider() const {
//   return custom_browser_view_;
// }

// void CustomBrowserWebUIView::CloseContainer() {
//   // SetContainerTargetVisibility(false,
//   WebUITabStripOpenCloseReason::kOther);
// }

void CustomBrowserWebUIView::SetContainerTargetVisibility(bool target_visible) {
  if (target_visible) {
    if (owned_contents_->IsCrashed()) {
      InitializeWebView(/*reattached=*/false);
    }

    immersive_revealed_lock_ =
        ImmersiveModeController::From(custom_browser_view_->browser())
            ->GetRevealedLock(ImmersiveModeController::ANIMATE_REVEAL_YES);

    SetVisible(true);
    PreferredSizeChanged();
    // Switch focus to the WebView container. This prevents a confusing
    // situation where a View appears to have focus, but keyboard inputs
    // are actually directed to the WebUITabStrip.
    web_view_->SetFocusBehavior(FocusBehavior::ALWAYS);
    web_view_->RequestFocus();
  } else {
    PreferredSizeChanged();
    SetVisible(false);
    web_view_->SetFocusBehavior(FocusBehavior::NEVER);
    immersive_revealed_lock_.reset();
  }
}

// void CustomBrowserWebUIView::ShowContextMenuAtPoint(
//     gfx::Point point,
//     std::unique_ptr<ui::MenuModel> menu_model,
//     base::RepeatingClosure on_menu_closed_callback) {
//   if (!web_view_->GetWebContents()) {
//     return;
//   }
//   ConvertPointToScreen(this, &point);
//   context_menu_model_ = std::move(menu_model);
//   int menu_runner_flags =
//       views::MenuRunner::HAS_MNEMONICS | views::MenuRunner::CONTEXT_MENU;
//   if (!base::FeatureList::IsEnabled(
//           features::kWebUITabStripContextMenuAfterTap)) {
//     menu_runner_flags |= views::MenuRunner::SEND_GESTURE_EVENTS_TO_OWNER;
//   }
//   context_menu_runner_ = std::make_unique<views::MenuRunner>(
//       context_menu_model_.get(), menu_runner_flags, on_menu_closed_callback);
//   context_menu_runner_->RunMenuAt(
//       GetWidget(), nullptr, gfx::Rect(point, gfx::Size()),
//       views::MenuAnchorPosition::kTopLeft, ui::mojom::MenuSourceType::kMouse,
//       web_view_->GetWebContents()->GetContentNativeView());
// }

// void CustomBrowserWebUIView::CloseContextMenu() {
//   if (!context_menu_runner_) {
//     return;
//   }
//   context_menu_runner_->Cancel();
// }

// void CustomBrowserWebUIView::ShowEditDialogForGroupAtPoint(
//     gfx::Point point,
//     gfx::Rect rect,
//     tab_groups::TabGroupId group) {
//   ConvertPointToScreen(this, &point);
//   rect.set_origin(point);
//   editor_bubble_widget_ = TabGroupEditorBubbleView::Show(
//       custom_browser_view_->browser(), group, nullptr, rect, this);
//   scoped_widget_observation_.Observe(editor_bubble_widget_.get());
// }

// void CustomBrowserWebUIView::HideEditDialogForGroup() {
//   if (editor_bubble_widget_) {
//     editor_bubble_widget_->CloseWithReason(
//         BrowserFrame::ClosedReason::kUnspecified);
//   }
// }

// BrowserWebUILayout CustomBrowserWebUIView::GetLayout() {
//   DCHECK(tab_contents_container_);

//   gfx::Size tab_contents_size = tab_contents_container_->size();

//   // Because some pages can display the bookmark bar even when the bookmark
//   bar
//   // is disabled (e.g. NTP) and some pages never display the bookmark bar
//   (e.g.
//   // crashed tab pages, pages in guest browser windows), we will always
//   reserve
//   // room for the bookmarks bar so that the size and shape of the effective
//   // viewport doesn't change.
//   //
//   // This may cause the thumbnail to crop off the extreme right and left edge
//   of
//   // the image in some cases, but a very slight crop is preferable to
//   constantly
//   // changing thumbnail sizes.
//   //
//   // See: crbug.com/1066652 for more info
//   const int max_bookmark_height = GetLayoutConstant(BOOKMARK_BAR_HEIGHT);
//   const views::View* bookmarks = custom_browser_view_->bookmark_bar();
//   const int bookmark_bar_height =
//       (bookmarks && bookmarks->GetVisible()) ? bookmarks->height() : 0;
//   tab_contents_size.Enlarge(0, -(max_bookmark_height - bookmark_bar_height));

//   return BrowserWebUILayout::CalculateForWebViewportSize(tab_contents_size);
// }

// SkColor CustomBrowserWebUIView::GetColorProviderColor(
//     ui::ColorId id) const {
//   return GetColorProvider()->GetColor(id);
// }

gfx::Size CustomBrowserWebUIView::CalculatePreferredSize(
    const views::SizeBounds& available_size) const {
  gfx::Size preferred_size =
      GetLayoutManager()->GetPreferredSize(this, available_size);

  return GetVisible() ? preferred_size : gfx::Size();
}

void CustomBrowserWebUIView::OnViewBoundsChanged(View* observed_view) {
  if (observed_view == custom_browser_view_) {
    // TODO(pbos): PreferredSizeChanged seems to cause infinite recursion with
    // BrowserView::ChildPreferredSizeChanged. InvalidateLayout here should be
    // replaceable with PreferredSizeChanged.
    InvalidateLayout();

    if (auto* custom_browser_webui = GetBrowserWebUI(owned_contents_.get())) {
      custom_browser_webui->LayoutChanged();
    }
  }
}

void CustomBrowserWebUIView::OnViewIsDeleting(View* observed_view) {
  view_observations_.RemoveObservation(observed_view);
}

void CustomBrowserWebUIView::OnWidgetDestroying(views::Widget* widget) {
  if (widget != editor_bubble_widget_) {
    return;
  }

  scoped_widget_observation_.Reset();
  editor_bubble_widget_ = nullptr;
}

void CustomBrowserWebUIView::AddedToWidget() {
  views::AccessiblePaneView::AddedToWidget();
  if (!reattached_pending_show_ || !owned_contents_) {
    return;
  }
  reattached_pending_show_ = false;
  // A parked page was hidden explicitly (ParkForBackground); the WebView's
  // own visibility plumbing does not reliably flip it back for an adopted
  // contents, so do what glic does when it shows its kept-alive client
  // (GlicInstanceImpl::MaybeShowHostUi): force VISIBLE now that the view is
  // in a widget. The guest <webview> follows its embedder's visibility.
  owned_contents_->UpdateWebContentsVisibility(content::Visibility::VISIBLE);
}

bool CustomBrowserWebUIView::SetPaneFocusAndFocusDefault() {
  // Make sure the pane first receives focus, then send a WebUI event to the
  // front-end so the correct HTML element receives focus.
  bool received_focus = AccessiblePaneView::SetPaneFocusAndFocusDefault();
  auto* custom_browser_webui = GetBrowserWebUI(owned_contents_.get());
  if (received_focus && custom_browser_webui) {
    custom_browser_webui->ReceivedKeyboardFocus();
  }
  return received_focus;
}

void CustomBrowserWebUIView::PrimaryMainFrameRenderProcessGone(
    base::TerminationStatus status) {
  static base::NoDestructor<base::AtomicFlag> restart_requested;
  // Since all WebUI tab strips are sharing the same render process, if the
  // render process has crashed, we close the container. When users click on
  // the tab counter button WebUI tab strip should be able to recover.
  //  CloseContainer();
  content::WebContentsObserver::Observe(nullptr);

  if (browser_shutdown::IsTryingToQuit()) {
    return;
  }

  if (status == base::TERMINATION_STATUS_NORMAL_TERMINATION ||
      status == base::TERMINATION_STATUS_STILL_RUNNING) {
    return;
  }

  if (restart_requested->IsSet()) {
    return;
  }
  restart_requested->Set();

  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&chrome::AttemptRestart));
}

void CustomBrowserWebUIView::DocumentOnLoadCompletedInPrimaryMainFrame() {
  // This method is called when the document in the primary main frame has
  // finished loading. We can use this opportunity to perform any necessary
  // updates to the tab strip UI.
  SetPaneFocusAndFocusDefault();
}

void CustomBrowserWebUIView::DidFinishNavigation(
    content::NavigationHandle* navigation_handle) {
  if (!navigation_handle->IsInPrimaryMainFrame() ||
      !navigation_handle->HasCommitted() ||
      navigation_handle->IsSameDocument()) {
    return;
  }
  // A reload (the recovery overlay's reloadPanel) commits a NEW WebUI
  // controller. InitializeWebView only handed this window to the first one,
  // so the new one would park its page handlers forever and
  // DeinitializeWebView would find it without a view. Give it this window.
  auto* custom_browser_webui = GetBrowserWebUI(owned_contents_.get());
  if (custom_browser_webui && !custom_browser_webui->HasView()) {
    custom_browser_webui->AttachToView(custom_browser_view_);
  }
}

// static
void CustomBrowserWebUIView::EnableDraggableRegions(
    content::WebContents* web_contents) {
  // Enables the web contents to support web platform defined draggable regions
  // for the current primary render frame host. This should be called each time
  // the primary rfh changes (after navigation for e.g.).
  if (!web_contents) {
    return;
  }
  web_contents->SetSupportsDraggableRegions(true);
}

void CustomBrowserWebUIView::InitializeWebView(bool reattached) {
  DCHECK(web_view_);
  DCHECK(owned_contents_);
  content::WebContentsObserver::Observe(owned_contents_.get());
  owned_contents_->SetDelegate(this);

  if (custom_browser_view_->IsNexusUIMode()) {
    // CanDragEnter admits file drags in this window (they are meant for the
    // guest's composer). One that misses the guest must not navigate the page
    // that hosts every session, so this page never loads a dropped file.
    // (The guest refuses load drops on its own.)
    owned_contents_->GetMutableRendererPrefs()->can_accept_load_drops = false;
    owned_contents_->SyncRendererPrefs();
  }

  if (reattached) {
    // The page is already loaded and mid-life (sessions running in its
    // guest). Load this window's layout params for the mode the window
    // decided on (what BuildInitUrl() does as a side effect) and re-point the
    // page's handlers at this window; no navigation.
    custom_browser_view_->ui_prefs()->RestoreFromPrefs();
    if (auto* custom_browser_webui = GetBrowserWebUI(owned_contents_.get())) {
      custom_browser_webui->AttachToView(custom_browser_view_);
    }
    if (auto* drag_state = static_cast<ParkedDragState*>(
            owned_contents_->GetUserData(kParkedDragStateKey))) {
      draggable_region_ = drag_state->draggable_region;
      is_draggable_ready_ = drag_state->is_draggable_ready;
      guest_drag_region_ = drag_state->guest_drag_region;
      guest_drag_ready_ = drag_state->guest_drag_ready;
      owned_contents_->RemoveUserData(kParkedDragStateKey);
    }
  } else {
    // Load WebUI with uiMode parameter from prefs.
    GURL initial_url = custom_browser_view_->ui_prefs()->BuildInitUrl();
    web_view_->LoadInitialURL(initial_url);
    task_manager::WebContentsTags::CreateForTabContents(owned_contents_.get());

    if (auto* custom_browser_webui = GetBrowserWebUI(owned_contents_.get())) {
      // References to the |custom_browser_view_->browser()| and |this| are
      // borrowed here, and will be released |DeinitializeWebView|.
      custom_browser_webui->Initialize(custom_browser_view_);
    }
  }

  // Enable draggable regions for window dragging support
  CustomBrowserWebUIView::EnableDraggableRegions(owned_contents_.get());
}

void CustomBrowserWebUIView::DeinitializeWebView() {
  content::WebContentsObserver::Observe(nullptr);

  if (auto* custom_browser_webui = GetBrowserWebUI(owned_contents_.get())) {
    // See corresponding comments from InitializeWebView().
    custom_browser_webui->Deinitialize();
  }
}

// void CustomBrowserWebUIView::UpdateHolderPosition(const std::string& name,
// const gfx::Rect& rect) {
//   if (name == "address_bar") {
//     // Update the address bar's position.
//   } else if (name == "web_contents") {
//     // Update the web contents' position.
//   }
// }

bool CustomBrowserWebUIView::CanDragEnter(
    content::WebContents* source,
    const content::DropData& data,
    blink::DragOperationsMask operations_allowed) {
  // The kNexus window admits OS file drags: they are aimed at the guest's
  // composer dropzone, and this delegate is asked on the guest's behalf.
  if (custom_browser_view_ && custom_browser_view_->IsNexusUIMode() &&
      custom_browser::IsNexusFileDrop(data)) {
    return true;
  }

  // TODO(crbug.com/40110968): Prevent dragging across Chromium instances.
  if (auto it = data.custom_data.find(kCustomWebUITabIdDataType);
      it != data.custom_data.end()) {
    int tab_id;
    bool found_tab_id = base::StringToInt(it->second, &tab_id);
    return found_tab_id &&
           extensions::ExtensionTabUtil::GetTabById(
               tab_id, custom_browser_view_->browser()->profile(), false,
               nullptr);
  }

  if (auto it = data.custom_data.find(kCustomWebUITabGroupIdDataType);
      it != data.custom_data.end()) {
    std::string group_id = base::UTF16ToUTF8(it->second);
    BrowserWindowInterface* found_browser = tab_strip_ui::GetBrowserWithGroupId(
        Profile::FromBrowserContext(custom_browser_view_->browser()->profile()),
        group_id);
    return found_browser != nullptr;
  }

  return false;
}

bool CustomBrowserWebUIView::IsPrivileged() {
  // WebContentsViewAura admits a drag only when source and target agree on
  // "privileged", so a privileged kNexus page would refuse every OS file drag
  // before CanDragEnter is even asked. The kNexus page has no tab-strip drags
  // to protect; like Glic's view it reports itself unprivileged, which also
  // keeps its own in-page drags (source and target both unprivileged) working.
  return !(custom_browser_view_ && custom_browser_view_->IsNexusUIMode());
}

void CustomBrowserWebUIView::DraggableRegionsChanged(
    const std::vector<blink::mojom::DraggableRegionPtr>& regions,
    content::WebContents* contents) {
  // For browser UI, draggable regions are important for defining which parts of
  // the UI can be used to drag the window
  if (contents == owned_contents_.get()) {
    // Store the updated draggable regions by directly converting to optimized
    // format
    SkRegion sk_region;
    for (const blink::mojom::DraggableRegionPtr& region : regions) {
      sk_region.op(
          SkIRect::MakeLTRB(region->bounds.x(), region->bounds.y(),
                            region->bounds.x() + region->bounds.width(),
                            region->bounds.y() + region->bounds.height()),
          region->draggable ? SkRegion::kUnion_Op : SkRegion::kDifference_Op);
    }

    draggable_region_ = sk_region;
    is_draggable_ready_ = true;  // Mark the view as ready for interaction
    // content::WebContents* wc = web_view_->GetWebContents();
    // auto v = wc->GetVisibility();  //
    // content::Visibility::{VISIBLE,HIDDEN,OCCLUDED} LOG(ERROR) << "WebUI
    // visibility = " << static_cast<int>(v)
    //           << " IsShowing=" << (wc->GetRenderWidgetHostView() &&
    //                               wc->GetRenderWidgetHostView()->IsShowing());
  }
}

gfx::Point CustomBrowserWebUIView::ConvertPointToWebUITarget(
    const View* source,
    const gfx::Point& point) const {
  // Convert the point from the source view's coordinate system to the WebUI's
  // coordinate system.
  gfx::Point converted_point(point);
  views::View::ConvertPointToTarget(source, web_view_.get(), &converted_point);
  return converted_point;
}

bool CustomBrowserWebUIView::IsInWebUIDragRegion(
    const gfx::Point& web_view_point) const {
  // First, check if the point is within the view bounds at all
  if (!GetLocalBounds().Contains(web_view_point)) {
    return false;  // The point is outside the view
  }

  if (custom_browser_view_->IsMaximized() ||
      custom_browser_view_->IsFullscreen()) {
    return false;
  }

  // Guest topbar drag map (React topbar inside the nexus <webview>). Its
  // `-webkit-app-region` never reaches this host frame, so it is reported via
  // SetGuestDragRegion instead. Checked first: a point on the bar drags even
  // though this frame's own `draggable_region_` is empty; a point on an
  // interactive control was subtracted out, so it falls through to a client
  // hit and the click reaches the control.
  if (guest_drag_ready_ &&
      guest_drag_region_.contains(web_view_point.x(), web_view_point.y())) {
    return true;
  }

  if (!is_draggable_ready_) {
    // If draggable regions are not ready, we cannot determine if the point is
    // in a draggable region.
    return true;  // Return client area hit test
  }

  return draggable_region_.contains(web_view_point.x(), web_view_point.y());
}

void CustomBrowserWebUIView::SetGuestDragRegion(
    const std::vector<gfx::Rect>& draggable,
    const std::vector<gfx::Rect>& subtract) {
  // Build the region exactly like DraggableRegionsChanged: union the draggable
  // bar rects, then difference out the interactive-control rects.
  SkRegion sk_region;
  for (const gfx::Rect& r : draggable) {
    sk_region.op(SkIRect::MakeLTRB(r.x(), r.y(), r.right(), r.bottom()),
                 SkRegion::kUnion_Op);
  }
  for (const gfx::Rect& r : subtract) {
    sk_region.op(SkIRect::MakeLTRB(r.x(), r.y(), r.right(), r.bottom()),
                 SkRegion::kDifference_Op);
  }
  guest_drag_region_ = sk_region;
  guest_drag_ready_ = true;
}

BEGIN_METADATA(CustomBrowserWebUIView)
END_METADATA
