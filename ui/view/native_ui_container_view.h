#ifndef CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_CONTAINER_VIEW_H_
#define CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_CONTAINER_VIEW_H_

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "base/containers/span.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "custom_browser/ui/mojom/ui_view_controller.mojom.h"
#include "custom_browser/ui/prefs/custom_browser_ui_prefs.h"
#include "custom_browser/ui/view/native_ui/native_ui.h"
#include "third_party/abseil-cpp/absl/types/variant.h"
#include "ui/gfx/geometry/size.h"
#include "ui/views/view.h"
#include "ui/views/view_observer.h"
#include "ui/views/view_targeter_delegate.h"

using custom_browser::mojom::NativeViewParams;
using custom_browser::mojom::NativeViewParamsPtr;

class CustomBrowserView;
class CustomBrowserUiPrefs;
namespace custom_browser {
class NexusModeButton;
}  // namespace custom_browser
namespace custom_browser::native_ui {
class CaptionContainerNativeUI;
}  // namespace custom_browser::native_ui

class NativeUIContainerView : public views::View,
                              public views::ViewObserver,
                              public CustomBrowserUiPrefs::Observer {
  METADATA_HEADER(NativeUIContainerView, views::View)
 public:
  NativeUIContainerView(CustomBrowserView* custom_browser_view);
  ~NativeUIContainerView() override;

  void Init(views::View* webui_view);

  void RestoreChromiumHierarchy();
  void SaveChromiumHierarchy();

  views::View* FindVisibleView(const gfx::Point& location) const;

  // CustomBrowserUiPrefs::Observer
  void OnLayoutUpdated(const NativeViewParams* added,
                       const NativeViewParams* removed) override;
  void OnLayoutAdded(const NativeViewParams* layout) override;
  void OnLayoutRemoved(const NativeViewParams* layout) override;
  void OnUIModeChanged(custom_browser::mojom::UIMode ui_mode) override;

  // views::View:
  void Layout(PassKey) override;

 private:
  std::unique_ptr<custom_browser::native_ui::NativeUI> CreateNativeUI(
      const NativeViewParams* layout_ptr);
  void EnsureNexusModeButton();

  template <typename T>
  T* FindNativeUI(const NativeViewParams* layout_ptr) const;

  views::View* SearchView(
      const views::View* root,
      const absl::variant<int, std::string>& selector) const;
  views::View* CreateView(const absl::variant<int, std::string>& selector);

  raw_ptr<views::View> webui_view_;
  raw_ptr<CustomBrowserView> custom_browser_view_;
  raw_ptr<CustomBrowserUiPrefs> browser_ui_prefs_;
  raw_ptr<custom_browser::NexusModeButton> nexus_mode_button_ = nullptr;

  std::vector<views::View*> view_cache_list_;
  std::vector<std::unique_ptr<custom_browser::native_ui::NativeUI>>
      native_ui_views_;

  bool layout_task_scheduled_ = false;
};

#endif  // CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_CONTAINER_VIEW_H_
