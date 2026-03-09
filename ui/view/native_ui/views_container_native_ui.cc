#include "custom_browser/ui/view/native_ui/views_container_native_ui.h"

#include <algorithm>
#include <unordered_map>
#include <utility>

#include "base/check.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "chrome/browser/ui/views/extensions/extensions_toolbar_desktop.h"
#include "chrome/browser/ui/views/frame/toolbar_button_provider.h"
#include "chrome/browser/ui/views/location_bar/location_bar_view.h"
#include "custom_browser/ui/mojom/ui_view_controller.mojom.h"
#include "custom_browser/ui/view/custom_browser_view.h"
#include "custom_browser/ui/view/native_ui/native_ui_utils.h"
#include "ui/views/view.h"

namespace custom_browser::native_ui {

ViewsContainerNativeUI::ViewsContainerNativeUI(
    views::View* host_view,
    const custom_browser::mojom::NativeViewParams* native_view_params)
    : NativeUI(host_view, native_view_params) {}

ViewsContainerNativeUI::~ViewsContainerNativeUI() {
  if (extensions_container_) {
    utils::StopObservingViewLayout(extensions_container_);
  }
}

void ViewsContainerNativeUI::OnPrepare() {
  EnsureChildViews();
}

void ViewsContainerNativeUI::OnLayout() {
  if (views_.empty()) {
    return;
  }
  UpdateViewsVisibility();

  const gfx::Rect container_bounds = bounds_;
  const int gap = ChildGap();
  const int total_gap = gap * std::max(0, static_cast<int>(views_.size()) - 1);
  const int available_width = std::max(0, container_bounds.width() - total_gap);

  std::vector<int> widths(views_.size());
  std::vector<int> heights(views_.size());
  int total_base_width = 0;
  int total_flex = 0;
  int extensions_index = -1;

  for (size_t i = 0; i < views_.size(); ++i) {
    const auto* view = GetViewAtIndex(i);
    if (view == extensions_container_) {
      extensions_index = static_cast<int>(i);
    }
    const auto* child_params = GetChildViewParamAt(i);
    const gfx::Size preferred_size = view->GetPreferredSize();
    const int width = child_params->width.has_value()
                          ? std::max(0, child_params->width.value())
                          : preferred_size.width();
    const int height = child_params->height.has_value()
                           ? std::max(0, child_params->height.value())
                           : preferred_size.height();
    widths[i] = width;
    heights[i] = height;
    total_base_width += width;
    const int flex = std::max(0, child_params->flex.value_or(0));
    if (flex > 0) {
      total_flex += flex;
    }
  }

  if (extensions_index != -1 && total_base_width > available_width) {
    const int other_width = total_base_width - widths[extensions_index];
    const int max_extensions_width =
        std::max(0, available_width - other_width);
    if (widths[extensions_index] > max_extensions_width) {
      widths[extensions_index] = max_extensions_width;
      total_base_width = other_width + widths[extensions_index];
    }
  }

  const int extra_space = std::max(0, available_width - total_base_width);
  if (extra_space > 0 && total_flex > 0) {
    int distributed = 0;
    int last_flex_index = -1;
    for (size_t i = 0; i < views_.size(); ++i) {
      const int flex = std::max(0, GetChildViewParamAt(i)->flex.value_or(0));
      if (flex <= 0) {
        continue;
      }
      last_flex_index = static_cast<int>(i);
      const int extra = (extra_space * flex) / total_flex;
      widths[i] += extra;
      distributed += extra;
    }
    if (last_flex_index != -1) {
      widths[last_flex_index] += (extra_space - distributed);
    }
  }

  int cursor_x = container_bounds.x();
  const int available_height = container_bounds.height();
  for (size_t i = 0; i < views_.size(); ++i) {
    auto* view = GetViewAtIndex(i);
    DCHECK(view);
    const int width = std::max(0, widths[i]);
    const int bounded_height =
        available_height > 0
            ? std::min(std::max(0, heights[i]), available_height)
            : std::max(0, heights[i]);
    const int vertical_space = available_height - bounded_height;
    const int y = container_bounds.y() + std::max(0, vertical_space / 2);

    view->SetBounds(cursor_x, y, width, bounded_height);

    cursor_x += width;
    if (gap > 0 && i + 1 < views_.size()) {
      cursor_x += gap;
    }
  }
}

const custom_browser::mojom::NativeChildView*
ViewsContainerNativeUI::GetChildViewParamAt(int index) const {
  DCHECK(views_.size() == layout_params_->child_views->size());
  if (!layout_params_->child_views.has_value()) {
    return nullptr;
  }
  const auto& child_params = *layout_params_->child_views;
  if (index < 0 || static_cast<size_t>(index) >= child_params.size()) {
    return nullptr;
  }
  return child_params[index].get();
}

bool ViewsContainerNativeUI::EnsureChildViews() {
  DCHECK(views_.empty() && layout_params_->child_views.has_value());
  child_bindings_.clear();
  const auto& child_params = *layout_params_->child_views;
  for (const auto& child_ptr : child_params) {
    DCHECK(child_ptr);
    auto* child_view = ResolveChildView(*child_ptr);
    if (child_view) {
      views_.push_back(child_view);
    } else {
      LOG(WARNING) << "ViewsContainerNativeUI: could not resolve child view."
                      " view_id="
                   << (child_ptr->view_id.has_value() ? *child_ptr->view_id
                                                      : "<none>")
                   << ", class_name="
                   << (child_ptr->view_class.has_value()
                           ? *child_ptr->view_class
                           : "<none>");
      views_.clear();
      return false;
    }
  }

  auto* toolbar_button_provider =
      GetCustomBrowserView()->toolbar_button_provider();
  auto* extensions_toolbar_container =
      toolbar_button_provider
          ? toolbar_button_provider->GetExtensionsToolbarDesktop()
          : nullptr;
  for (auto& view : views_) {
    bool is_location_bar = view == GetCustomBrowserView()->GetLocationBarView();
    views::View* parent = is_location_bar ? host_view()->parent() : host_view();
    DCHECK(parent);
    parent->AddChildView(view);
    if (view == extensions_toolbar_container) {
      extensions_container_ = view;
      utils::StartObservingViewLayout(this, view);
    }
  }
  return true;
}

views::View* ViewsContainerNativeUI::ResolveChildView(
    const custom_browser::mojom::NativeChildView& child) const {
  views::View* view = nullptr;
  if (child.view_id.has_value() && !child.view_id->empty()) {
    int parsed_id = 0;
    if (base::StringToInt(*child.view_id, &parsed_id)) {
      view = FindViewById(parsed_id);
    }
  }

  if (!view && child.view_class.has_value() && !child.view_class->empty()) {
    view = FindViewByClass(*child.view_class);
  }
  return view;
}

int ViewsContainerNativeUI::ChildGap() const {
  if (!layout_params_ || !layout_params_->gap.has_value()) {
    return 0;
  }
  return std::max(0, *layout_params_->gap);
}

}  // namespace custom_browser::native_ui
