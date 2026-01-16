#ifndef CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_NATIVE_UI_REGISTRATION_H_
#define CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_NATIVE_UI_REGISTRATION_H_

#include <memory>
#include <string_view>
#include <type_traits>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "custom_browser/ui/view/native_ui/native_ui_registration_macros.h"

namespace custom_browser::native_ui {

namespace native_ui_registration_internal {

template <typename T, typename = void>
struct HasNativeClassName : std::false_type {};

template <typename T>
struct HasNativeClassName<T, std::void_t<decltype(T::NativeClassName())>>
    : std::true_type {};

template <typename T, typename = void>
struct HasNativeClassNames : std::false_type {};

template <typename T>
struct HasNativeClassNames<T, std::void_t<decltype(T::NativeClassNames())>>
    : std::true_type {};

template <typename T, typename = void>
struct HasNativeViewIds : std::false_type {};

template <typename T>
struct HasNativeViewIds<T, std::void_t<decltype(T::NativeViewIds())>>
    : std::true_type {};

template <typename T>
struct DependentFalse : std::false_type {};

template <typename T>
std::unique_ptr<NativeUI> CreateWithMatchedView(
    views::View* host_view,
    const custom_browser::mojom::NativeViewParams* params,
    views::View* matched_view) {
  return std::make_unique<T>(host_view, params, matched_view);
}

template <typename T>
std::unique_ptr<NativeUI> CreateWithoutMatchedView(
    views::View* host_view,
    const custom_browser::mojom::NativeViewParams* params) {
  return std::make_unique<T>(host_view, params);
}

}  // namespace native_ui_registration_internal

template <typename T>
inline void NativeUIFactoryRegistry::RegisterType() {
  namespace internal = native_ui_registration_internal;
  constexpr bool kHasClassName = internal::HasNativeClassName<T>::value;
  constexpr bool kHasClassNames = internal::HasNativeClassNames<T>::value;
  constexpr bool kHasIds = internal::HasNativeViewIds<T>::value;
  static_assert(kHasClassName || kHasClassNames || kHasIds,
                "NativeUI types must expose a class name or view ids");

  const base::span<const std::string_view> class_names =
      [&]() -> base::span<const std::string_view> {
    if constexpr (kHasClassNames) {
      return T::NativeClassNames();
    } else if constexpr (kHasClassName) {
      static const std::string_view kNames[] = {T::NativeClassName()};
      return base::span<const std::string_view>(kNames);
    }
    return {};
  }();

  if constexpr (std::is_constructible_v<
                    T, views::View*,
                    const custom_browser::mojom::NativeViewParams*,
                    views::View*>) {
    if (!class_names.empty()) {
      for (const std::string_view name : class_names) {
        if (name.empty()) {
          continue;
        }
        RegisterFactoryForClassName(
            name, base::BindRepeating(&internal::CreateWithMatchedView<T>));
      }
    }
    if constexpr (kHasIds) {
      for (int view_id : T::NativeViewIds()) {
        RegisterFactoryForViewId(
            view_id, base::BindRepeating(&internal::CreateWithMatchedView<T>));
      }
    }
  } else if constexpr (std::is_constructible_v<
                           T, views::View*,
                           const custom_browser::mojom::NativeViewParams*>) {
    if (!class_names.empty()) {
      for (const std::string_view name : class_names) {
        if (name.empty()) {
          continue;
        }
        RegisterFactoryForClassName(
            name, base::BindRepeating(&internal::CreateWithoutMatchedView<T>));
      }
    }
    if constexpr (kHasIds) {
      static_assert(internal::DependentFalse<T>::value,
                    "NativeUI types without matched-view constructor cannot "
                    "register ids");
    }
  } else {
    static_assert(internal::DependentFalse<T>::value,
                  "Unsupported constructor for NativeUI registration");
  }
}

}  // namespace custom_browser::native_ui

#endif  // CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_NATIVE_UI_REGISTRATION_H_
