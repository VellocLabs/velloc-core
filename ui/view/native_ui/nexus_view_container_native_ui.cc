#include "custom_browser/ui/view/native_ui/nexus_view_container_native_ui.h"

#include "base/check.h"
#include "custom_browser/browser/nexus/view/nexus_view_container.h"
#include "custom_browser/ui/view/custom_browser_view.h"
#include "custom_browser/ui/view/native_ui/native_ui_utils.h"

namespace custom_browser::native_ui {

NexusViewContainerNativeUI::NexusViewContainerNativeUI(
    views::View* host_view,
    const custom_browser::mojom::NativeViewParams* layout_params)
    : NativeUI(host_view, layout_params) {}

NexusViewContainerNativeUI::~NexusViewContainerNativeUI() = default;

void NexusViewContainerNativeUI::OnPrepare() {
  DCHECK(layout_params_->view_class_name.has_value());
  DCHECK(views().empty());

  const std::string& view_class_name = *layout_params_->view_class_name;
  auto* view = utils::FindUnusedView(host_view(), view_class_name);
  if (!view) {
    view = new nexus::NexusViewContainer(GetCustomBrowserView()->GetProfile());
  }

  AddView(view);
  host_view()->AddChildView(view);
  utils::MarkViewUsed(view, view_class_name);
}

void NexusViewContainerNativeUI::OnLayout() {
  DCHECK(view());
  UpdateViewsVisibility();
  if (visible()) {
    view()->SetBoundsRect(bounds());
  }
}

}  // namespace custom_browser::native_ui
