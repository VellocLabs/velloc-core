#ifndef CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_PINNED_TOOLBAR_ACTIONS_CONTAINER_NATIVE_UI_H_
#define CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_PINNED_TOOLBAR_ACTIONS_CONTAINER_NATIVE_UI_H_

#include <string_view>

#include "base/memory/weak_ptr.h"
#include "chrome/browser/ui/views/toolbar/pinned_toolbar_actions_container.h"
#include "custom_browser/ui/view/native_ui/native_ui.h"
#include "custom_browser/ui/view/native_ui/native_ui_registration_macros.h"
#include "ui/views/view_observer.h"

namespace custom_browser::native_ui {

class PinnedToolbarActionsContainerNativeUI : public ViewNativeUI {
 public:
  DECLARE_NATIVE_UI_CLASS_NAME(PinnedToolbarActionsContainerNativeUI,
                               PinnedToolbarActionsContainer::kViewClassName)

  PinnedToolbarActionsContainerNativeUI(
      views::View* host_view,
      const custom_browser::mojom::NativeViewParams* layout_params,
      views::View* view);
  ~PinnedToolbarActionsContainerNativeUI() override;

  static std::optional<actions::ActionId> GetActionIdFromClassName(
      const std::string& class_name);

 protected:
  void OnPrepare() override;
};

}  // namespace custom_browser::native_ui

#endif  // CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_PINNED_TOOLBAR_ACTIONS_CONTAINER_NATIVE_UI_H_
