#include "custom_browser/ui/view/native_ui_container_view.h"

#include <algorithm>
#include <optional>
#include <utility>
#include <variant>

#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/ui/actions/chrome_action_id.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/views/extensions/extensions_toolbar_desktop.h"
#include "chrome/browser/ui/views/frame/multi_contents_view.h"
#include "chrome/browser/ui/views/frame/tab_strip_region_view.h"
#include "chrome/browser/ui/views/frame/top_container_view.h"
#include "chrome/browser/ui/views/location_bar/location_bar_view.h"
#include "chrome/browser/ui/ui_features.h"
#include "chrome/browser/ui/views/tabs/tab_search_container.h"
#include "chrome/browser/ui/views/toolbar/pinned_toolbar_actions_container.h"
#include "chrome/browser/ui/views/toolbar/toolbar_view.h"
#include "custom_browser/ui/mojom/browser_constants.mojom.h"
#include "custom_browser/ui/prefs/custom_browser_ui_prefs.h"
#include "custom_browser/ui/view/custom_browser_view.h"
#include "custom_browser/ui/view/native_ui/create_toolbar_button_native_ui.h"
#include "custom_browser/ui/view/native_ui/main_contents_native_ui.h"
#include "custom_browser/ui/view/native_ui/native_ui_factory_registry.h"
#include "custom_browser/ui/view/native_ui/native_ui_utils.h"
#include "custom_browser/ui/view/native_ui/navigation_container_native_ui.h"
#include "custom_browser/ui/view/view_debug.h"
#include "ui/actions/action_id.h"
#include "ui/actions/actions.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/views/debug_utils.h"

namespace {
bool IsSearchViewStopped(const views::View* v) {
  const auto class_name = v->GetClassName();
  if (class_name == LocationBarView::kViewClassName) {
    return true;
  }
  if (class_name.ends_with("Button")) {
    return true;
  }
  return false;
}
}  // namespace

NativeUIContainerView::NativeUIContainerView(
    CustomBrowserView* custom_browser_view)
    : custom_browser_view_(custom_browser_view),
      browser_ui_prefs_(custom_browser_view->ui_prefs()) {
  DCHECK(custom_browser_view && browser_ui_prefs_);
  // Observe preference changes to mirror UI updates initiated elsewhere.
  browser_ui_prefs_->AddObserver(this);
}

NativeUIContainerView::~NativeUIContainerView() {
  // Remove ourselves as observer from any tracked views to be safe.
  for (auto* v : view_cache_list_) {
    if (v) {
      v->RemoveObserver(this);
    }
  }
  if (browser_ui_prefs_) {
    browser_ui_prefs_->RemoveObserver(this);
  }
}

void NativeUIContainerView::Init(views::View* webui_view) {
  webui_view_ = webui_view;
}

void NativeUIContainerView::RestoreChromiumHierarchy() {
  native_ui_views_.clear();
  custom_browser::native_ui::utils::RestoreChromiumHierarchy(
      custom_browser_view_);
}

void NativeUIContainerView::SaveChromiumHierarchy() {
  DCHECK(native_ui_views_.empty());
  custom_browser::native_ui::utils::SaveChromiumHierarchy(custom_browser_view_);
}

void NativeUIContainerView::Layout(PassKey) {
  for (auto& native_ui : native_ui_views_) {
    native_ui->CalculateBounds();
    native_ui->Layout();
  }
  // LayoutSuperclass<views::View>(this);
}

views::View* NativeUIContainerView::FindVisibleView(
    const gfx::Point& location) const {
  for (const auto& native_ui : native_ui_views_) {
    if (!native_ui) {
      continue;
    }
    if (!native_ui->visible()) {
      continue;
    }
    auto& views = native_ui->views();
    for (const auto& view : views) {
      if (view->GetVisible() && native_ui->bounds().Contains(location)) {
        return view.get();
      }
    }
  }
  return nullptr;
}

views::View* NativeUIContainerView::SearchView(
    const views::View* view,
    const absl::variant<int, std::string>& selector) const {
  bool match = false;
  if (const int* id = absl::get_if<int>(&selector)) {
    match = (view->GetID() == *id);
  } else if (const std::string* name = absl::get_if<std::string>(&selector)) {
    match = (view->GetClassName() == *name);
  }
  if (match) {
    return const_cast<views::View*>(view);
  }
  if (IsSearchViewStopped(view)) {
    return nullptr;
  }
  for (const auto& child : view->children()) {
    if (auto* found = SearchView(child.get(), selector)) {
      return found;
    }
  }
  return nullptr;
}

template <typename T>
T* NativeUIContainerView::FindNativeUI(
    const NativeViewParams* native_view_params) const {
  for (const auto& native_ui : native_ui_views_) {
    if (native_ui->layout_params() == native_view_params) {
      return static_cast<T*>(native_ui.get());
    }
  }
  return nullptr;
}

void NativeUIContainerView::OnUIModeChanged(
    custom_browser::mojom::UIMode ui_mode) {
  custom_browser::native_ui::utils::PrintViewHierarchyLog(custom_browser_view_);
}

void NativeUIContainerView::OnLayoutUpdated(
    const NativeViewParams* old_params,
    const NativeViewParams* new_params) {
  auto* native_ui =
      FindNativeUI<custom_browser::native_ui::NativeUI>(old_params);
  DCHECK(native_ui) << "No NativeUI found for updated layout id="
                    << old_params->hash_id;
  if (native_ui) {
    native_ui->Update(new_params);
    native_ui->CalculateBounds();
    native_ui->Layout();
  }
}

void NativeUIContainerView::OnLayoutAdded(
    const NativeViewParams* native_view_params) {
  // Implementation for OnLayoutAdded
  auto native_ui = CreateNativeUI(native_view_params);
  if (!native_ui) {
    native_ui =
        custom_browser::native_ui::NativeUIFactoryRegistry::GetInstance()
            .MaybeCreateNoopForLayout(this, native_view_params);
  }

  if (native_ui) {
    native_ui->Prepare();
    native_ui->CalculateBounds();
    native_ui->Layout();
    native_ui_views_.push_back(std::move(native_ui));
  }
}

void NativeUIContainerView::OnLayoutRemoved(const NativeViewParams* layout) {
  // Implementation for OnLayoutRemoved
  auto it = std::remove_if(
      native_ui_views_.begin(), native_ui_views_.end(),
      [layout](const std::unique_ptr<custom_browser::native_ui::NativeUI>&
                   native_ui) { return native_ui->layout_params() == layout; });
  native_ui_views_.erase(it, native_ui_views_.end());
}

std::unique_ptr<custom_browser::native_ui::NativeUI>
NativeUIContainerView::CreateNativeUI(
    const NativeViewParams* native_view_params) {
  DCHECK(native_view_params);
  DCHECK(
      !FindNativeUI<custom_browser::native_ui::NativeUI>(native_view_params));
  std::unique_ptr<custom_browser::native_ui::NativeUI> native_ui =
      custom_browser::native_ui::NativeUIFactoryRegistry::GetInstance()
          .MaybeCreateForLayout(this, native_view_params);
  if (!native_ui) {
    const auto& lp = *native_view_params;
    absl::variant<int, std::string> selector;
    if (lp.view_id.has_value()) {
      int parsed_view_id = 0;
      base::StringToInt(*lp.view_id, &parsed_view_id);
      selector = parsed_view_id;
    } else if (lp.view_class_name.has_value()) {
      selector = *lp.view_class_name;
    } else {
      LOG(ERROR) << "LayoutParams has neither view_id nor view_class_name, "
                    "cannot create NativeUI";
      return nullptr;
    }

    views::View* view = SearchView(custom_browser_view_->parent(), selector);
    if (!view) {
      LOG(ERROR) << "No view found for layout ";
      return nullptr;
    }
    native_ui =
        custom_browser::native_ui::NativeUIFactoryRegistry::GetInstance()
            .MaybeCreateForLayout(this, native_view_params, view);
  }
  if (!native_ui) {
    LOG(ERROR) << "No factory found for layout ";
    return nullptr;
  }
  return native_ui;
}

BEGIN_METADATA(NativeUIContainerView)
END_METADATA
