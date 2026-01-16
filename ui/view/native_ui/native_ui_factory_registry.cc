#include "custom_browser/ui/view/native_ui/native_ui_factory_registry.h"

#include "chrome/browser/ui/views/bookmarks/bookmark_bar_view.h"
#include "chrome/browser/ui/views/extensions/extensions_toolbar_container.h"
#include "chrome/browser/ui/views/tabs/tab_search_button.h"
#include "chrome/browser/ui/views/tabs/tab_search_container.h"
#include "chrome/browser/ui/views/tabs/tab_strip_control_button.h"
#include "chrome/browser/ui/views/toolbar/pinned_toolbar_actions_container.h"
#include "chrome/browser/ui/views/toolbar/split_tabs_button.h"
#include "custom_browser/ui/mojom/browser_constants.mojom.h"
#include "custom_browser/ui/mojom/ui_view_controller.mojom.h"
#include "custom_browser/ui/view/native_ui/caption_container_native_ui_win.h"
#include "custom_browser/ui/view/native_ui/create_toolbar_button_native_ui.h"
#include "custom_browser/ui/view/native_ui/location_bar_auto_size_native_ui.h"
#include "custom_browser/ui/view/native_ui/main_contents_native_ui.h"
#include "custom_browser/ui/view/native_ui/native_ui.h"
#include "custom_browser/ui/view/native_ui/new_tab_button_native_ui.h"
#include "custom_browser/ui/view/native_ui/pinned_toolbar_actions_container_native_ui.h"
#include "custom_browser/ui/view/native_ui/tab_search_container_native_ui.h"
#include "custom_browser/ui/view/native_ui/views_container_native_ui.h"

namespace custom_browser::native_ui {

namespace {

REGISTER_NATIVE_UI_CLASS(MainContentsNativeUI)
REGISTER_NATIVE_UI_CLASS(PinnedToolbarActionsContainerNativeUI)
REGISTER_NATIVE_UI_CLASS(ViewsContainerNativeUI)
REGISTER_NATIVE_UI_CLASS(CreateToolbarButtonNativeUI)
REGISTER_NATIVE_UI_CLASS(NewTabButtonNativeUI)
#if BUILDFLAG(IS_WIN)
REGISTER_NATIVE_UI_CLASS(CaptionContainerNativeUI)
#endif
REGISTER_NATIVE_UI_CLASS(LocationBarAutoSizeNativeUI)

REGISTER_VIEW_NATIVE_UI_ADAPTER(BookmarkBarView)
REGISTER_DYNAMIC_VIEW_NATIVE_UI_ADAPTER(ExtensionsToolbarContainer)
REGISTER_VIEW_NATIVE_UI_ADAPTER(SplitTabsToolbarButton)
REGISTER_NATIVE_UI_CLASS(TabSearchContainerNativeUI)

REGISTER_VIEW_NATIVE_UI_IDS_ADAPTER(ToolbarButtonView,
                                    1001,
                                    1002,
                                    1003,
                                    1004,
                                    1007,
                                    1018)

}  // namespace

NativeUIFactoryRegistry& NativeUIFactoryRegistry::GetInstance() {
  static base::NoDestructor<NativeUIFactoryRegistry> instance;
  return *instance;
}

NativeUIFactoryRegistry::NativeUIFactoryRegistry() = default;

NativeUIFactoryRegistry::~NativeUIFactoryRegistry() = default;

bool NativeUIFactoryRegistry::IsRegisteredForView(views::View* view) const {
  DCHECK(view);
  if (IsRegisteredForViewId(view->GetID())) {
    return true;
  }

  if (IsRegisteredForClassName(view->GetClassName())) {
    return true;
  }
  return false;
}

void NativeUIFactoryRegistry::RegisterFactoryForClassName(
    std::string_view class_name,
    FactoryNoViewCallback factory) {
  std::string key(class_name);
  DCHECK(!IsRegisteredForClassName(key) && !factory.is_null());
  class_name_no_view_factories_[std::move(key)] = std::move(factory);
}

void NativeUIFactoryRegistry::RegisterFactoryForClassName(
    std::string_view class_name,
    FactoryCallback factory) {
  std::string key(class_name);
  DCHECK(!IsRegisteredForClassName(key) && !factory.is_null());
  class_name_factories_[std::move(key)] = std::move(factory);
}

void NativeUIFactoryRegistry::RegisterFactoryForViewId(
    int view_id,
    FactoryCallback factory) {
  DCHECK(!IsRegisteredForViewId(view_id) && !factory.is_null());
  id_factories_[view_id] = std::move(factory);
}

NativeUIFactoryRegistry::FactoryCallback
NativeUIFactoryRegistry::GetFactoryByClassName(
    std::string_view class_name) const {
  std::string key(class_name);
  auto it = class_name_factories_.find(key);
  if (it != class_name_factories_.end()) {
    return it->second;
  }
  return FactoryCallback();
}
NativeUIFactoryRegistry::FactoryNoViewCallback
NativeUIFactoryRegistry::GetFactoryNoViewByClassName(
    std::string_view class_name) const {
  std::string key(class_name);
  auto it = class_name_no_view_factories_.find(key);
  if (it != class_name_no_view_factories_.end()) {
    return it->second;
  }
  return FactoryNoViewCallback();
}

NativeUIFactoryRegistry::FactoryCallback
NativeUIFactoryRegistry::GetFactoryByViewId(int view_id) const {
  auto it = id_factories_.find(view_id);
  if (it != id_factories_.end()) {
    return it->second;
  }
  return FactoryCallback();
}

std::unique_ptr<NativeUI> NativeUIFactoryRegistry::MaybeCreateForLayout(
    views::View* host_view,
    const custom_browser::mojom::NativeViewParams* native_view_params) const {
  if (!native_view_params->view_class_name.has_value()) {
    return nullptr;
  }
  auto factory =
      GetFactoryNoViewByClassName(*native_view_params->view_class_name);
  if (!factory.is_null()) {
    return factory.Run(host_view, native_view_params);
  }

  return nullptr;
}

std::unique_ptr<NativeUI> NativeUIFactoryRegistry::MaybeCreateForLayout(
    views::View* host_view,
    const custom_browser::mojom::NativeViewParams* native_view_params,
    views::View* matched_view) const {
  DCHECK(matched_view);
  if (native_view_params->view_id.has_value()) {
    auto factory = GetFactoryByViewId(matched_view->GetID());
    if (!factory.is_null()) {
      return factory.Run(host_view, native_view_params, matched_view);
    }
  } else if (native_view_params->view_class_name.has_value()) {
    auto factory = GetFactoryByClassName(*native_view_params->view_class_name);
    if (!factory.is_null()) {
      return factory.Run(host_view, native_view_params, matched_view);
    }
  }
  return nullptr;
}

std::unique_ptr<NativeUI> NativeUIFactoryRegistry::MaybeCreateNoopForLayout(
    views::View* host_view,
    const custom_browser::mojom::NativeViewParams* native_view_params) const {
  return std::make_unique<NoopNativeUI>(host_view, native_view_params);
}

}  // namespace custom_browser::native_ui
