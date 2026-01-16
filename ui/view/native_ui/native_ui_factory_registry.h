#ifndef CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_FACTORY_REGISTRY_H_
#define CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_FACTORY_REGISTRY_H_

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

#include "base/containers/flat_map.h"
#include "base/functional/callback.h"
#include "base/no_destructor.h"

namespace custom_browser::mojom {
class NativeViewParams;
}  // namespace custom_browser::mojom

namespace views {
class View;
}  // namespace views

class CustomBrowserView;

namespace custom_browser::native_ui {

class NativeUI;

class NativeUIFactoryRegistry {
 public:
  using FactoryCallback = base::RepeatingCallback<std::unique_ptr<NativeUI>(
      views::View*,
      const custom_browser::mojom::NativeViewParams*,
      views::View*)>;

  using FactoryNoViewCallback = base::RepeatingCallback<std::unique_ptr<
      NativeUI>(views::View*, const custom_browser::mojom::NativeViewParams*)>;

  static NativeUIFactoryRegistry& GetInstance();

  void RegisterFactoryForClassName(std::string_view class_name,
                                   FactoryCallback factory);
  void RegisterFactoryForViewId(int view_id, FactoryCallback factory);
  void RegisterFactoryForClassName(std::string_view class_name,
                                   FactoryNoViewCallback factory);

  bool IsRegisteredForViewId(int view_id) const {
    return !GetFactoryByViewId(view_id).is_null();
  }

  bool IsRegisteredForClassName(std::string_view class_name) const {
    return !GetFactoryByClassName(class_name).is_null() ||
           !GetFactoryNoViewByClassName(class_name).is_null();
  }

  bool IsRegisteredForView(views::View* view) const;

  std::unique_ptr<NativeUI> MaybeCreateForLayout(
      views::View* host_view,
      const custom_browser::mojom::NativeViewParams* native_view_params) const;

  std::unique_ptr<NativeUI> MaybeCreateForLayout(
      views::View* host_view,
      const custom_browser::mojom::NativeViewParams* native_view_params,
      views::View* matched_view) const;

  std::unique_ptr<NativeUI> MaybeCreateNoopForLayout(
      views::View* host_view,
      const custom_browser::mojom::NativeViewParams* native_view_params) const;

  template <typename T>
  void RegisterType();

 private:
  friend base::NoDestructor<NativeUIFactoryRegistry>;
  NativeUIFactoryRegistry();
  ~NativeUIFactoryRegistry();

  FactoryCallback GetFactoryByClassName(std::string_view class_name) const;
  FactoryNoViewCallback GetFactoryNoViewByClassName(
      std::string_view class_name) const;
  FactoryCallback GetFactoryByViewId(int view_id) const;

  base::flat_map<std::string, FactoryCallback> class_name_factories_;
  base::flat_map<std::string, FactoryNoViewCallback>
      class_name_no_view_factories_;
  base::flat_map<int, FactoryCallback> id_factories_;
};

}  // namespace custom_browser::native_ui

#include "custom_browser/ui/view/native_ui/native_ui_registration.h"

#endif  // CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_FACTORY_REGISTRY_H_
