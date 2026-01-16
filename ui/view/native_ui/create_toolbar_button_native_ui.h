#ifndef CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_CREATE_TOOLBAR_BUTTON_NATIVE_UI_H_
#define CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_CREATE_TOOLBAR_BUTTON_NATIVE_UI_H_

#include <memory>
#include <string_view>

#include "custom_browser/ui/mojom/browser_constants.mojom-forward.h"
#include "custom_browser/ui/view/native_ui/native_ui.h"
#include "custom_browser/ui/view/native_ui/native_ui_registration_macros.h"
#include "ui/actions/action_id.h"

class PinnedToolbarActionsContainer;

namespace custom_browser::native_ui {

class CreateToolbarButtonNativeUI : public NativeUI {
 public:
  CreateToolbarButtonNativeUI(
      views::View* host_view,
      const custom_browser::mojom::NativeViewParams* layout_params);
  ~CreateToolbarButtonNativeUI() override;

  DECLARE_NATIVE_UI_CLASS_NAMES(CreateToolbarButtonNativeUI,
                                "PinnedShowDownloadsButton",
                                "PinnedTabSearchButton",
                                "DividerView",
                                "DividerView-Dark")
  views::View* CreatePinnedButtonForAction(const std::string& class_name);
 protected:
  void OnPrepare() override;
  void OnLayout() override;
 private:
   bool is_download_button_ = false;
};

}  // namespace custom_browser::native_ui

#endif  // CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_CREATE_TOOLBAR_BUTTON_NATIVE_UI_H_
