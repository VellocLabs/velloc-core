#include "custom_browser/ui/view/native_ui/create_toolbar_button_native_ui.h"

#include "base/check.h"
#include "base/strings/string_util.h"
#include "chrome/browser/ui/views/toolbar/pinned_toolbar_actions_container.h"
#include "chrome/browser/ui/views/toolbar/toolbar_view.h"
#include "custom_browser/ui/mojom/ui_view_controller.mojom.h"
#include "custom_browser/ui/view/custom_browser_view.h"
#include "custom_browser/ui/view/native_ui/native_ui_utils.h"
#include "custom_browser/ui/view/native_ui/pinned_toolbar_actions_container_native_ui.h"
#include "ui/views/view.h"

namespace custom_browser::native_ui {

CreateToolbarButtonNativeUI::CreateToolbarButtonNativeUI(
    views::View* host_view,
    const custom_browser::mojom::NativeViewParams* layout_params)
    : NativeUI(host_view, layout_params) {}

CreateToolbarButtonNativeUI::~CreateToolbarButtonNativeUI() {
  if (is_download_button_) {
    utils::SaveDownloadButton(GetCustomBrowserView(), nullptr);
  }
}

views::View* CreateToolbarButtonNativeUI::CreatePinnedButtonForAction(
    const std::string& class_name) {
  auto action_id_opt =
      PinnedToolbarActionsContainerNativeUI::GetActionIdFromClassName(
          class_name);
  auto* actions_container =
      GetCustomBrowserView()->toolbar()->pinned_toolbar_actions_container();
  if (!action_id_opt.has_value() || !actions_container) {
    return nullptr;
  }
  return actions_container->CreateOrGetButtonForAction(*action_id_opt)
      .release();
}

void CreateToolbarButtonNativeUI::OnPrepare() {
  DCHECK(layout_params_->view_class_name.has_value());
  const auto& class_name = *layout_params_->view_class_name;
  auto* view = utils::FindUnusedView(host_view(), class_name);
  if (!view) {
    if (base::StartsWith(class_name, "Pinned")) {
      view = CreatePinnedButtonForAction(class_name);
    } else if (base::StartsWith(class_name, "DividerView")) {
      view =
          utils::CreateDividerView(class_name == "DividerView-Dark").release();
    }
  }

  if (class_name == "PinnedShowDownloadsButton") {
    is_download_button_ = true;
    utils::SaveDownloadButton(GetCustomBrowserView(),
                              static_cast<ToolbarButton*>(view));
  }
  AddView(view);
  host_view()->AddChildView(view);
  utils::MarkViewUsed(view, class_name);
  DCHECK(views_.size() == 1);
}

void CreateToolbarButtonNativeUI::OnLayout() {
  DCHECK(view());
  UpdateViewsVisibility();
  if (visible()) {
    view()->SetBoundsRect(bounds_);
  }
}

}  // namespace custom_browser::native_ui
