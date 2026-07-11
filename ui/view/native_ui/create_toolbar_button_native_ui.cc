#include "custom_browser/ui/view/native_ui/create_toolbar_button_native_ui.h"

#include "base/check.h"
#include "base/strings/string_util.h"
#include "chrome/browser/ui/ui_features.h"
#include "chrome/browser/ui/views/toolbar/pinned_toolbar_actions_container.h"
#include "chrome/browser/ui/views/toolbar/toolbar_button.h"
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
  // custom-browser: Chromium 149 made ToolbarView's container member private
  // and exposes the PinnedToolbarActions base via GetPinnedToolbarActions().
  // In the custom browser configuration this is always the concrete
  // PinnedToolbarActionsContainer (which owns CreateOrGetButtonForAction()).
  auto* actions_container = static_cast<PinnedToolbarActionsContainer*>(
      GetCustomBrowserView()
          ->toolbar_button_provider()
          ->GetPinnedToolbarActions());
  if (!action_id_opt.has_value() || !actions_container) {
    return nullptr;
  }
  return actions_container->CreateOrGetButtonForAction(*action_id_opt)
      .release();
}

void CreateToolbarButtonNativeUI::OnPrepare() {
  DCHECK(layout_params_->view_class_name.has_value());
  const std::string& class_name = *layout_params_->view_class_name;
  std::string effective_class_name = class_name;
  auto* view = utils::FindUnusedView(host_view(), effective_class_name);
  if (!view) {
    if (base::StartsWith(effective_class_name, "Pinned")) {
      view = CreatePinnedButtonForAction(effective_class_name);
    } else if (base::StartsWith(effective_class_name, "DividerView")) {
      view =
          utils::CreateDividerView(
              effective_class_name == "DividerView-Dark").release();
    }
  }

  if (effective_class_name == "PinnedShowDownloadsButton") {
    is_download_button_ = true;
    utils::SaveDownloadButton(GetCustomBrowserView(),
                              static_cast<ToolbarButton*>(view));
  }
  if (effective_class_name == "PinnedTabSearchButton" && view) {
    GetCustomBrowserView()->SetActiveTabSearchButton(view);
  }
  AddView(view);
  host_view()->AddChildView(view);
  utils::MarkViewUsed(view, effective_class_name);
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
