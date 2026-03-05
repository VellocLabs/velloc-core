#include "custom_browser/ui/view/native_ui/caption_container_native_ui_win.h"

#include "base/check.h"
#include "chrome/browser/ui/views/frame/browser_frame_view_win.h"
#include "chrome/browser/ui/views/frame/tab_strip_region_view.h"
#include "chrome/browser/ui/views/tabs/tab_strip_action_container.h"
#include "chrome/browser/ui/views/frame/windows_caption_button.h"
#include "custom_browser/ui/view/custom_browser_view.h"
#include "custom_browser/ui/view/native_ui/native_ui_factory_registry.h"
#include "custom_browser/ui/view/native_ui/native_ui_utils.h"
#include "ui/aura/window.h"
#include "ui/base/hit_test.h"
#include "ui/compositor/layer.h"
#include "ui/events/event.h"
#include "ui/events/event_handler.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/views/controls/button/button.h"
#include "ui/views/controls/separator.h"
#include "ui/views/view.h"
#include "ui/views/view_utils.h"
#include "ui/views/widget/widget.h"
#include "ui/wm/core/coordinate_conversion.h"

namespace {
constexpr int kActionAreaPadding = 0;
}  // namespace

namespace custom_browser::native_ui {

CaptionContainerNativeUI::CaptionContainerNativeUI(
    views::View* host_view,
    const custom_browser::mojom::NativeViewParams* layout_params,
    views::View* view)
    : ViewNativeUI(host_view, layout_params, view) {}

CaptionContainerNativeUI::~CaptionContainerNativeUI() {
  view_observation_.Reset();
}

void CaptionContainerNativeUI::OnPrepare() {
  InitializeCaptionButtonContainer(
      views::AsViewClass<BrowserCaptionButtonContainer>(view()));
  AttachTabStripActionContainer();
}

void CaptionContainerNativeUI::OnLayout() {
  const bool is_visible = visible();
  if (view()) {
    view()->SetVisible(is_visible);
  }

  // Position action container to the left of the caption container
  // in the frame view coordinate space.
  TabStripActionContainer* tab_strip_action_container =
      tab_strip_action_container_.view();
  if (tab_strip_action_container && view()) {
    const gfx::Rect caption_bounds = view()->bounds();
    int container_w = tab_strip_action_container->GetPreferredSize().width();
    if (container_w <= 0) {
      container_w = tab_strip_action_container->bounds().width();
    }

    const bool show_action_container =
        is_visible && !caption_bounds.IsEmpty() && container_w > 0;
    tab_strip_action_container->SetVisible(show_action_container);
    if (!show_action_container) {
      return;
    }

    container_w = tab_strip_action_container->GetPreferredSize().width();
    if (container_w <= 0) {
      container_w = tab_strip_action_container->bounds().width();
    }

    const int container_x =
        caption_bounds.x() - (container_w + kActionAreaPadding);
    const int container_y = caption_bounds.y();
    const int container_h = caption_bounds.height();
    tab_strip_action_container->SetBounds(container_x, container_y, container_w,
                                          container_h);
    tab_strip_action_container->DeprecatedLayoutImmediately();
  } else {
    if (tab_strip_action_container) {
      tab_strip_action_container->SetVisible(false);
    }
  }
}

void CaptionContainerNativeUI::OnViewIsDeleting(views::View* observed_view) {
  if (view() != observed_view)
    return;

  view_observation_.Reset();
  if (!views_.empty())
    views_[0] = nullptr;

  if (BrowserCaptionButtonContainer* new_container =
          FindCaptionButtonContainer()) {
    if (new_container != observed_view)
      InitializeCaptionButtonContainer(new_container);
  }
}

void CaptionContainerNativeUI::InitializeCaptionButtonContainer(
    BrowserCaptionButtonContainer* container) {
  if (!container)
    return;

  if (!views_.empty())
    views_[0] = container;
  caption_button_container_ = container;
  caption_button_container_.EnsureLayer();
  caption_button_container_.SetFillsBoundsOpaquely(false);
  caption_button_container_.SetMasksToBounds(true);
  caption_button_container_.SetNotifyEnterExitOnChild(true);  // Stable hover
  view_observation_.Reset();
  view_observation_.Observe(container);
}

BrowserCaptionButtonContainer*
CaptionContainerNativeUI::FindCaptionButtonContainer() const {
  CustomBrowserView* custom_browser_view = GetCustomBrowserView();
  if (!custom_browser_view)
    return nullptr;

  views::View* frame_view = custom_browser_view->parent();
  while (frame_view && !views::IsViewClass<BrowserFrameViewWin>(frame_view)) {
    frame_view = frame_view->parent();
  }

  if (!frame_view)
    return nullptr;

  for (views::View* child : frame_view->children()) {
    if (auto* caption_container =
            views::AsViewClass<BrowserCaptionButtonContainer>(child)) {
      return caption_container;
    }
  }
  return nullptr;
}

void CaptionContainerNativeUI::AttachTabStripActionContainer() {
  if (tab_strip_action_container_) {
    return;
  }
  views::View* frame_view = view()->parent();
  if (!frame_view) {
    return;
  }

  auto* tab_strip_region_view = utils::FindTabStripRegionView(GetCustomBrowserView());
  if (!tab_strip_region_view) {
    return;
  }

 tab_strip_action_container_ =
      tab_strip_region_view->tab_strip_action_container_;
  if (!tab_strip_action_container_) {
    return;
  }
  tab_strip_action_container_.TemporarilyClearReference(
          &tab_strip_region_view->tab_strip_action_container_);
  tab_strip_action_container_.SetParent(frame_view);
  tab_strip_action_container_.EnsureLayer();
  tab_strip_action_container_.SetFillsBoundsOpaquely(false);
  tab_strip_action_container_.SetIgnoredByLayout(true);
}

}  // namespace custom_browser::native_ui
