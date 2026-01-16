#ifndef CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_NATIVE_UI_REGISTRATION_MACROS_H_
#define CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_NATIVE_UI_REGISTRATION_MACROS_H_

#include <string_view>

#include "base/containers/span.h"

namespace custom_browser::native_ui {
class NativeUIFactoryRegistry;
class ViewNativeUI;
class DynamicViewNativeUI;
}  // namespace custom_browser::native_ui

#define DECLARE_NATIVE_UI_CLASS_NAME(CLASS, NAME_LITERAL) \
  static constexpr std::string_view NativeClassName() {   \
    return NAME_LITERAL;                                  \
  }

#define DECLARE_NATIVE_UI_CLASS_NAMES(CLASS, ...)                \
  static base::span<const std::string_view> NativeClassNames() { \
    static const std::string_view kClassNames[] = {__VA_ARGS__}; \
    return base::span<const std::string_view>(kClassNames);      \
  }

#define DECLARE_NATIVE_UI_CLASS_NAME_FROM_VIEW(CLASS, VIEW_TYPE) \
  static constexpr std::string_view NativeClassName() {          \
    return VIEW_TYPE::kViewClassName;                            \
  }

#define REGISTER_NATIVE_UI_CLASS(CLASS)                             \
  namespace {                                                       \
  [[maybe_unused]] const bool CLASS##_native_ui_registered = []() { \
    NativeUIFactoryRegistry::GetInstance().RegisterType<CLASS>();   \
    return true;                                                    \
  }();                                                              \
  }

#define REGISTER_VIEW_NATIVE_UI_IDS_ADAPTER(VIEW_TYPE, ...)                  \
  namespace {                                                                \
  class VIEW_TYPE##NativeUIAdapter final : public ViewNativeUI {             \
   public:                                                                   \
    using ViewNativeUI::ViewNativeUI;                                        \
    static base::span<const int> NativeViewIds() {                           \
      static constexpr int kIds[] = {__VA_ARGS__};                           \
      return base::span<const int>(kIds);                                    \
    }                                                                        \
  };                                                                         \
  [[maybe_unused]] const bool VIEW_TYPE##NativeUIAdapter_registered = []() { \
    NativeUIFactoryRegistry::GetInstance()                                   \
        .RegisterType<VIEW_TYPE##NativeUIAdapter>();                         \
    return true;                                                             \
  }();                                                                       \
  }

#define REGISTER_VIEW_NATIVE_UI_ADAPTER(VIEW_TYPE)                           \
  namespace {                                                                \
  class VIEW_TYPE##NativeUIAdapter final : public ViewNativeUI {             \
   public:                                                                   \
    using ViewNativeUI::ViewNativeUI;                                        \
    static constexpr std::string_view NativeClassName() {                    \
      return VIEW_TYPE::kViewClassName;                                      \
    }                                                                        \
  };                                                                         \
  [[maybe_unused]] const bool VIEW_TYPE##NativeUIAdapter_registered = []() { \
    NativeUIFactoryRegistry::GetInstance()                                   \
        .RegisterType<VIEW_TYPE##NativeUIAdapter>();                         \
    return true;                                                             \
  }();                                                                       \
  }

#define REGISTER_DYNAMIC_VIEW_NATIVE_UI_ADAPTER(VIEW_TYPE)                   \
  namespace {                                                                \
  class VIEW_TYPE##NativeUIAdapter final : public DynamicViewNativeUI {      \
   public:                                                                   \
    using DynamicViewNativeUI::DynamicViewNativeUI;                          \
    static constexpr std::string_view NativeClassName() {                    \
      return VIEW_TYPE::kViewClassName;                                      \
    }                                                                        \
  };                                                                         \
  [[maybe_unused]] const bool VIEW_TYPE##NativeUIAdapter_registered = []() { \
    NativeUIFactoryRegistry::GetInstance()                                   \
        .RegisterType<VIEW_TYPE##NativeUIAdapter>();                         \
    return true;                                                             \
  }();                                                                       \
  }
#endif  // CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_NATIVE_UI_REGISTRATION_MACROS_H_
