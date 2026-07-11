#ifndef CUSTOM_BROWSER_UI_VIEW_MAIN_CONTENTS_NATIVE_UI_H_
#define CUSTOM_BROWSER_UI_VIEW_MAIN_CONTENTS_NATIVE_UI_H_

#include <memory>
#include <string_view>

#include "custom_browser/ui/mojom/browser_constants.mojom-forward.h"
#include "custom_browser/ui/view/native_ui/native_ui.h"
#include "custom_browser/ui/view/native_ui/native_ui_registration_macros.h"
#include "ui/color/color_id.h"

namespace custom_browser::native_ui {

class DrawCornerOverlay;

class MainContentsNativeUI : public NativeUI {
 public:
  MainContentsNativeUI(
      views::View* host_view,
      const custom_browser::mojom::NativeViewParams* layout_params);
  ~MainContentsNativeUI() override;

  DECLARE_NATIVE_UI_CLASS_NAME(MainContentsNativeUI,
                               custom_browser::mojom::kMainContentsViewName)
  void OnLayout() override;
 private:
  void EnsureOverlay(const ui::ColorId& color_id, int radius);
  std::unique_ptr<DrawCornerOverlay> corner_overlay_;
};

}  // namespace custom_browser::native_ui

#endif  // CUSTOM_BROWSER_UI_VIEW_MAIN_CONTENTS_NATIVE_UI_H_
