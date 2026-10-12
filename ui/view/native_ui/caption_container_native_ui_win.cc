#include "custom_browser/ui/view/native_ui/caption_container_native_ui_win.h"

#include "base/check.h"
#include "chrome/browser/ui/views/frame/browser_frame_view_win.h"
#include "chrome/browser/ui/views/frame/horizontal_tab_strip_region_view.h"
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
// constexpr int kActionAreaPadding = 0;
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
  // AttachTabStripActionContainer();
}

void CaptionContainerNativeUI::OnLayout() {
  const bool is_visible = visible();
  if (view()) {
    view()->SetVisible(is_visible);
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

}  // namespace custom_browser::native_ui
