#ifndef CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_VIEWS_CONTAINER_NATIVE_UI_H_
#define CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_VIEWS_CONTAINER_NATIVE_UI_H_

#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "custom_browser/ui/mojom/browser_constants.mojom-forward.h"
#include "custom_browser/ui/mojom/ui_view_controller.mojom-forward.h"
#include "custom_browser/ui/view/native_ui/native_ui.h"
#include "custom_browser/ui/view/native_ui/native_ui_registration_macros.h"
#include "ui/views/view.h"

namespace custom_browser::mojom {
class NativeViewParams;
class NativeChildView;
}  // namespace custom_browser::mojom

namespace custom_browser::native_ui {

class ViewsContainerNativeUI : public NativeUI {
 public:
  ViewsContainerNativeUI(
      views::View* host_view,
      const custom_browser::mojom::NativeViewParams* layout_params);
  ~ViewsContainerNativeUI() override;

  DECLARE_NATIVE_UI_CLASS_NAME(ViewsContainerNativeUI,
                               custom_browser::mojom::kViewsContainerViewName)
  void OnPrepare() override;
  void OnLayout() override;

 private:
  struct ChildBinding {
    raw_ptr<const custom_browser::mojom::NativeChildView> params = nullptr;
    raw_ptr<views::View> view = nullptr;
  };

  bool EnsureChildViews();
  const custom_browser::mojom::NativeChildView* GetChildViewParamAt(
      int index) const;

  views::View* ResolveChildView(
      const custom_browser::mojom::NativeChildView& child) const;
  int ChildGap() const;

  raw_ptr<views::View> extensions_container_ = nullptr;
  std::vector<ChildBinding> child_bindings_;
};

}  // namespace custom_browser::native_ui

#endif  // CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_VIEWS_CONTAINER_NATIVE_UI_H_
