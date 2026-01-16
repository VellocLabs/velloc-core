#ifndef CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_NEW_TAB_BUTTON_NATIVE_UI_H_
#define CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_NEW_TAB_BUTTON_NATIVE_UI_H_

#include "custom_browser/ui/mojom/browser_constants.mojom-forward.h"
#include "custom_browser/ui/view/native_ui/native_ui.h"
#include "custom_browser/ui/view/native_ui/native_ui_registration_macros.h"

class ToolbarButton;

// Native wrapper for the New Tab toolbar button that may react to focus.
namespace custom_browser::native_ui {

class NewTabButtonNativeUI : public NativeUI {
 public:
  NewTabButtonNativeUI(views::View* host_view,
                       const custom_browser::mojom::NativeViewParams* params);
  NewTabButtonNativeUI(const NewTabButtonNativeUI&) = delete;
  NewTabButtonNativeUI& operator=(const NewTabButtonNativeUI&) = delete;
  ~NewTabButtonNativeUI() override;
  DECLARE_NATIVE_UI_CLASS_NAME(
      NewTabButtonNativeUI,
      custom_browser::mojom::kNewTabToolbarButtonViewName)

 protected:
  void OnPrepare() override;
  void OnLayout() override;

 private:
  ToolbarButton* GetNewTabButton() const;
};

}  // namespace custom_browser::native_ui

#endif  // CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_NEW_TAB_BUTTON_NATIVE_UI_H_
