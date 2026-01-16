#include "custom_browser/ui/web_ui/custom_browser_webui_view.h"

#include <utility>

#include "base/feature_list.h"
#include "base/functional/callback.h"
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
#include "chrome/browser/lifetime/browser_shutdown.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/task_manager/web_contents_tags.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/ui_features.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/views/frame/immersive_mode_controller.h"
#include "chrome/browser/ui/views/tabs/tab_group_editor_bubble_view.h"
#include "chrome/browser/ui/webui/tab_strip/tab_strip_ui_util.h"
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
#include "custom_browser/ui/prefs/custom_browser_ui_prefs.h"
#include "custom_browser/ui/view/custom_browser_view.h"
#include "custom_browser/ui/web_ui/custom_browser_constants.h"
#include "custom_browser/ui/web_ui/custom_browser_webui.h"
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
  // TODO(crbug.com/40651211) WebContents are initially assumed to be visible by
  // default unless explicitly hidden. The WebContents need to be set to hidden
  // so that the visibility state of the document in JavaScript is correctly
  // initially set to 'hidden', and the 'visibilitychange' events correctly get
  // fired.
  web_view_->GetWebContents()->WasHidden();

  web_view_->set_allow_accelerators(true);

  SetLayoutManager(std::make_unique<views::FillLayout>());
  InitializeWebView();

  SetContainerTargetVisibility(true);
  // SetPaneFocusAndFocusDefault();
}

CustomBrowserWebUIView::~CustomBrowserWebUIView() {
  DeinitializeWebView();
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
  return web_view_->GetWebContents();
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
    if (web_view_->GetWebContents()->IsCrashed()) {
      InitializeWebView();
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

    if (auto* custom_browser_webui =
            GetBrowserWebUI(web_view_->GetWebContents())) {
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

bool CustomBrowserWebUIView::SetPaneFocusAndFocusDefault() {
  // Make sure the pane first receives focus, then send a WebUI event to the
  // front-end so the correct HTML element receives focus.
  bool received_focus = AccessiblePaneView::SetPaneFocusAndFocusDefault();
  auto* custom_browser_webui = GetBrowserWebUI(web_view_->GetWebContents());
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

void CustomBrowserWebUIView::InitializeWebView() {
  DCHECK(web_view_);
  content::WebContentsObserver::Observe(web_view_->GetWebContents());
  web_view_->GetWebContents()->SetDelegate(this);

  // Load WebUI with uiMode parameter from prefs.
  GURL initial_url = custom_browser_view_->ui_prefs()->BuildInitUrl();
  web_view_->LoadInitialURL(initial_url);
  task_manager::WebContentsTags::CreateForTabContents(
      web_view_->web_contents());

  if (auto* custom_browser_webui =
          GetBrowserWebUI(web_view_->GetWebContents())) {
    // References to the |custom_browser_view_->browser()| and |this| are
    // borrowed here, and will be released |DeinitializeWebView|.
    custom_browser_webui->Initialize(custom_browser_view_);
  }

  // Enable draggable regions for window dragging support
  CustomBrowserWebUIView::EnableDraggableRegions(web_view_->GetWebContents());
}

void CustomBrowserWebUIView::DeinitializeWebView() {
  content::WebContentsObserver::Observe(nullptr);

  if (auto* custom_browser_webui =
          GetBrowserWebUI(web_view_->GetWebContents())) {
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
  return true;
}

void CustomBrowserWebUIView::DraggableRegionsChanged(
    const std::vector<blink::mojom::DraggableRegionPtr>& regions,
    content::WebContents* contents) {
  // For browser UI, draggable regions are important for defining which parts of
  // the UI can be used to drag the window
  if (contents == web_view_->GetWebContents()) {
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

  if (!is_draggable_ready_) {
    // If draggable regions are not ready, we cannot determine if the point is
    // in a draggable region.
    return true;  // Return client area hit test
  }

  return draggable_region_.contains(web_view_point.x(), web_view_point.y());
}

BEGIN_METADATA(CustomBrowserWebUIView)
END_METADATA
