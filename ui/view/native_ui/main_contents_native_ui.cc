#include "custom_browser/ui/view/native_ui/main_contents_native_ui.h"

#include "chrome/browser/ui/views/bookmarks/bookmark_bar_view.h"
#include "chrome/browser/ui/views/frame/layout/browser_view_layout.h"
#include "custom_browser/ui/view/custom_browser_view.h"
#include "custom_browser/ui/view/custom_browser_view_layout.h"
#include "custom_browser/ui/view/native_ui/draw_corner_overlay.h"
#include "custom_browser/ui/view/native_ui/native_ui_factory_registry.h"
#include "custom_browser/ui/view/webui_color_resolver.h"


namespace custom_browser::native_ui {

MainContentsNativeUI::MainContentsNativeUI(
    views::View* host_view,
    const custom_browser::mojom::NativeViewParams* native_view_params)
    : NativeUI(host_view, native_view_params) {
  const int radius =
      (native_view_params && native_view_params->radius.has_value())
          ? *native_view_params->radius
          : 0;
  std::optional<ui::ColorId> overlay_color;
  if (native_view_params && native_view_params->background_color.has_value()) {
    overlay_color = webui_color_resolver::ResolveChromeColorId(
        *native_view_params->background_color);
    if (overlay_color) {
      EnsureOverlay(*overlay_color, radius);
    }
  }
}

MainContentsNativeUI::~MainContentsNativeUI() {
}

void MainContentsNativeUI::OnLayout() {
  if (corner_overlay_) {
    corner_overlay_->Relayout(&bounds_);
  }
  GetCustomBrowserView()
      ->GetCustomBrowserViewLayout()
      ->LayoutContentsContainerView(bounds_);
}

void MainContentsNativeUI::EnsureOverlay(const ui::ColorId& color_id,
                                         int radius) {
  if (!corner_overlay_) {
    corner_overlay_ = DrawCornerOverlay::Create(GetCustomBrowserView(),
                                                host_view(),
                                                DrawCornerOverlay::kAll);
  }

  DCHECK(corner_overlay_);
  corner_overlay_->SetCornerRadius(radius);
  corner_overlay_->SetSeparatorColor(color_id);
}

}  // namespace custom_browser::native_ui
