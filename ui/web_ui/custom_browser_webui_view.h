#ifndef CUSTOM_BROWSER_UI_WEB_UI_CUSTOM_BROWSER_WEBUI_VIEW_H_
#define CUSTOM_BROWSER_UI_WEB_UI_CUSTOM_BROWSER_WEBUI_VIEW_H_

#include <memory>
#include <optional>
#include <set>
#include <vector>

// Base includes
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/process/process_handle.h"
#include "base/scoped_multi_source_observation.h"
#include "base/scoped_observation.h"


// Content includes for WebContentsDelegate/Observer
#include "content/public/browser/eye_dropper.h"
#include "content/public/browser/keyboard_event_processing_result.h"
#include "content/public/browser/media_stream_request.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents_delegate.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/common/referrer.h"


// UI includes
#include "components/input/native_web_keyboard_event.h"
#include "ui/base/accelerators/accelerator.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
#include "ui/color/color_id.h"
#include "ui/gfx/animation/animation_delegate.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"
#include "ui/views/accessible_pane_view.h"
#include "ui/views/view.h"
#include "ui/views/view_observer.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_observer.h"


// Mojo includes
#include "third_party/blink/public/mojom/choosers/file_chooser.mojom.h"
#include "third_party/blink/public/mojom/page/draggable_region.mojom.h"
#include "third_party/blink/public/mojom/window_features/window_features.mojom.h"


// Custom browser includes
#include "components/tab_groups/tab_group_id.h"
#include "custom_browser/ui/web_ui/custom_browser_webui_embedder.h"
#include "third_party/skia/include/core/SkRegion.h"
#include "ui/gfx/canvas.h"


namespace ui {
class AcceleratorProvider;
class MenuModel;
}  // namespace ui

namespace views {
class MenuRunner;
class NativeViewHost;
class WebView;
}  // namespace views

class Browser;
class CustomBrowserView;
class ImmersiveRevealedLock;

class CustomBrowserWebUIView :  // public CustomBrowserUIEmbedder,
                                public gfx::AnimationDelegate,
                                public views::AccessiblePaneView,
                                public views::ViewObserver,
                                public views::WidgetObserver,
                                public content::WebContentsDelegate,
                                public content::WebContentsObserver {
  METADATA_HEADER(CustomBrowserWebUIView, views::AccessiblePaneView)

 public:
  CustomBrowserWebUIView(CustomBrowserView* browser_view);
  ~CustomBrowserWebUIView() override;

  static bool SupportsTouchableTabStrip(const Browser* browser);
  static bool UseTouchableTabStrip(const Browser* browser);

  // For drag-and-drop support:
  static void GetDropFormatsForView(
      int* formats,
      std::set<ui::ClipboardFormatType>* format_types);
  static bool IsDraggedTab(const ui::OSExchangeData& data);

  void OpenForTabDrag();

  views::NativeViewHost* GetNativeViewHost();
  content::WebContents* GetWebContents();

  // Returns true if the point is in a draggable region.
  bool IsInWebUIDragRegion(const gfx::Point& web_view_point) const;

  // Sets the window-drag map reported by the React topbar inside the nexus
  // guest <webview>. `-webkit-app-region` in the guest never reaches this
  // (host) frame's DraggableRegionsChanged, so the guest measures its bar +
  // controls and sends them here via `ui.setWindowDragRegions`. `draggable`
  // rects union into the region; `subtract` rects (interactive controls) are
  // removed so clicks still reach them. Coords are in this view's space (the
  // guest topbar sits at the view origin). ORed with `draggable_region_` in
  // IsInWebUIDragRegion.
  void SetGuestDragRegion(const std::vector<gfx::Rect>& draggable,
                          const std::vector<gfx::Rect>& subtract);

  gfx::Point ConvertPointToWebUITarget(const View* source,
                                       const gfx::Point& point) const;

  // Clicking the tab counter button opens and closes the container with
  // an animation, so it is unsuitable for an interactive test. This
  // should be called instead. View::SetVisible() isn't sufficient since
  // the container's preferred size will change.
  void SetVisibleForTesting(bool visible);
  views::WebView* web_view_for_testing() const { return web_view_; }

  // Finish the open or close animation if it's active.
  void FinishAnimationForTesting();

  // content::WebContentsObserver:
  void PrimaryMainFrameRenderProcessGone(
      base::TerminationStatus status) override;
  void DocumentOnLoadCompletedInPrimaryMainFrame() override;
  void DidFinishNavigation(
      content::NavigationHandle* navigation_handle) override;

 private:
  void SetContainerTargetVisibility(bool target_visible);

  // `reattached`: the contents came back from nexus::NexusBackgroundPanelHost
  // with its page already loaded — re-point the WebUI at this window instead
  // of navigating. False for a fresh contents and for the crash-reload path.
  void InitializeWebView(bool reattached);
  void DeinitializeWebView();

  // Background panel (System > "Continue running background apps when main
  // window is closed"): a kNexus window's contents is parked with the
  // profile's NexusBackgroundPanelHost instead of dying with the window.
  bool ShouldParkForBackground() const;
  void ParkForBackground();

  // CustomBrowserUIEmbedder:
  //   const ui::AcceleratorProvider* GetAcceleratorProvider() const override;
  //   void CloseContainer() override;
  //   void ShowContextMenuAtPoint(
  //       gfx::Point point,
  //       std::unique_ptr<ui::MenuModel> menu_model,
  //       base::RepeatingClosure on_menu_closed_callback) override;
  //   void CloseContextMenu() override;
  //   void ShowEditDialogForGroupAtPoint(gfx::Point point,
  //                                      gfx::Rect rect,
  //                                      tab_groups::TabGroupId group)
  //                                      override;
  //   void HideEditDialogForGroup() override;

  // content::WebContentsDelegate:
  bool CanDragEnter(content::WebContents* source,
                    const content::DropData& data,
                    blink::DragOperationsMask operations_allowed) override;
  bool IsPrivileged() override;
  void DraggableRegionsChanged(
      const std::vector<blink::mojom::DraggableRegionPtr>& regions,
      content::WebContents* contents) override;

  // CustomBrowserWebUILayout GetLayout() override;
  //   SkColor GetColorProviderColor(ui::ColorId id) const override;

  // views::View:
  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override;

  // views::ViewObserver:
  void OnViewBoundsChanged(views::View* observed_view) override;
  void OnViewIsDeleting(views::View* observed_view) override;

  // views::WidgetObserver:
  void OnWidgetDestroying(views::Widget* widget) override;

  // views::AccessiblePaneView
  bool SetPaneFocusAndFocusDefault() override;

  // views::View
  void AddedToWidget() override;

  // Helper method to enable draggable regions in the web contents
  static void EnableDraggableRegions(content::WebContents* web_contents);

  const raw_ptr<CustomBrowserView> custom_browser_view_;
  const raw_ptr<views::WebView> web_view_;
  // The top-chrome WebContents. Owned here (not by `web_view_`, which only
  // displays it) so it can be handed to NexusBackgroundPanelHost when the
  // window closes, and taken back from it when the next kNexus window opens.
  std::unique_ptr<content::WebContents> owned_contents_;
  // Set when `owned_contents_` came back parked (hidden) and must be shown
  // explicitly once this view is in a widget — see AddedToWidget().
  bool reattached_pending_show_ = false;
  bool is_draggable_ready_ = false;
  // Used to keep the toolbar revealed while the tab strip is open.
  std::unique_ptr<ImmersiveRevealedLock> immersive_revealed_lock_;

  std::unique_ptr<views::MenuRunner> context_menu_runner_;
  std::unique_ptr<ui::MenuModel> context_menu_model_;

  base::ScopedMultiSourceObservation<views::View, views::ViewObserver>
      view_observations_{this};
  base::ScopedObservation<views::Widget, views::WidgetObserver>
      scoped_widget_observation_{this};

  raw_ptr<views::Widget> editor_bubble_widget_ = nullptr;

  // Store the draggable regions defined by the WebUI content
  // Used for window dragging support with web platform draggable regions
  // Optimized storage as gfx::Rect for faster hit testing
  SkRegion draggable_region_;

  // Window-drag map reported by the nexus guest topbar (SetGuestDragRegion).
  // Separate from `draggable_region_` (which only ever reflects this host
  // frame's own `-webkit-app-region`); both are consulted by
  // IsInWebUIDragRegion so the guest topbar can drag the frameless window.
  SkRegion guest_drag_region_;
  bool guest_drag_ready_ = false;
};

#endif  // CUSTOM_BROWSER_UI_WEB_UI_CUSTOM_BROWSER_WEBUI_VIEW_H_
