#include "custom_browser/ui/view/native_ui/native_ui.h"

#include <algorithm>
#include <unordered_set>

#include "base/check.h"
#include "custom_browser/ui/mojom/ui_view_controller.mojom.h"
#include "custom_browser/ui/view/custom_browser_view.h"
#include "custom_browser/ui/view/native_ui/native_ui_utils.h"
#include "ui/compositor/layer.h"
#include "ui/views/view.h"


namespace custom_browser::native_ui {

NativeUI::NativeUI(views::View* host_view,
                   const NativeViewParams* layout_params)
    : host_view_(host_view) {
  DCHECK(host_view_);
  Update(layout_params);
}

NativeUI::~NativeUI() {
  for (const auto& view : views_) {
    if (view) {
      utils::ReleaseView(view);
    }
  }
}

views::View* NativeUI::FindViewById(int id) const {
  return GetCustomBrowserView()->parent()->GetViewByID(id);
}

views::View* NativeUI::FindViewByClass(const std::string& class_name) const {
  return utils::FindViewByClassInHierarchy(GetCustomBrowserView()->parent(),
                                           class_name);
}

void NativeUI::Update(const NativeViewParams* layout_params) {
  DCHECK(layout_params && layout_params_ != layout_params);
  layout_params_ = layout_params;
}

void NativeUI::CalculateBounds() {
#if DCHECK_IS_ON()
  if (LogInfo()) {
    LOG(INFO) << "CalculateBounds - Start: view_class_name = "
              << layout_params_->view_class_name.value_or("<unnamed>")
              << " view_id = " << layout_params_->view_id.value_or("<unknown>");
  }
#endif

  const auto& params = *layout_params_->params;
  const gfx::Rect host = host_view_->GetLocalBounds();
  const int host_width = host.width();
  const int host_height = host.height();

  const bool has_left = params.left.has_value();
  const bool has_right = params.right.has_value();
  const bool has_top = params.top.has_value();
  const bool has_bottom = params.bottom.has_value();

  int width = params.width.has_value() ? *params.width : 0;
  if (!params.width.has_value() && has_left && has_right) {
    width = std::max(0, host_width - *params.left - *params.right);
  }

  int height = params.height.has_value() ? *params.height : 0;
  if (!params.height.has_value() && has_top && has_bottom) {
    height = std::max(0, host_height - *params.top - *params.bottom);
  }

  int x = 0;
  if (has_left) {
    x = *params.left;
  } else if (has_right) {
    x = host_width - *params.right - width;
  }
  if (params.width.has_value() && has_left && has_right) {
    x = *params.left;
  }

  int y = 0;
  if (has_top) {
    y = *params.top;
  } else if (has_bottom) {
    y = host_height - *params.bottom - height;
  }
  if (params.height.has_value() && has_top && has_bottom) {
    y = *params.top;
  }

  auto host_bounds = gfx::Rect(x, y, width, height);
  bounds_ = CalculateAlignmentBounds(host_bounds);
#if DCHECK_IS_ON()
  if (LogInfo()) {
    LOG(INFO) << "CalculateBounds - Complete: "
              << " host bounds = " << host_bounds.ToString()
              << " Final bounds = " << bounds_.ToString();
  }
#endif
}

#if DCHECK_IS_ON()
bool NativeUI::LogInfo() const {
  return false;
}
#endif

gfx::Size NativeUI::GetPreferredSize(
    const views::SizeBounds& size_bounds) const {
  if (views_.size() == 1) {
    return view()->GetPreferredSize(size_bounds);
  }
  return gfx::Size();
}

gfx::Rect NativeUI::CalculateAlignmentBounds(
    const gfx::Rect& available_bounds) const {
  gfx::Rect adjusted = available_bounds;
  const auto& params = *layout_params_;
  const auto preferred_size =
      GetPreferredSize(views::SizeBounds(available_bounds.size()));
  if (preferred_size.IsEmpty()) {
    return adjusted;
  }
  // Horizontal alignment
  if (params.h_align.has_value()) {
    adjusted.set_width(preferred_size.width());
    switch (*params.h_align) {
      case custom_browser::mojom::LayoutAlignment::kStart:
        break;
      case custom_browser::mojom::LayoutAlignment::kEnd:
        adjusted.set_x(available_bounds.right() - preferred_size.width());
        break;
      case custom_browser::mojom::LayoutAlignment::kCenter:
        adjusted.set_x(available_bounds.x() +
                       (available_bounds.width() - preferred_size.width()) / 2);
        break;
      case custom_browser::mojom::LayoutAlignment::kFill:
        adjusted.set_width(available_bounds.width());
        break;
    }
  }
  // Vertical alignment
  if (params.v_align.has_value()) {
    adjusted.set_height(preferred_size.height());
    switch (*params.v_align) {
      case custom_browser::mojom::LayoutAlignment::kStart:
        break;
      case custom_browser::mojom::LayoutAlignment::kEnd:
        adjusted.set_y(available_bounds.bottom() - preferred_size.height());
        break;
      case custom_browser::mojom::LayoutAlignment::kCenter:
        adjusted.set_y(available_bounds.y() +
                       (available_bounds.height() - preferred_size.height()) /
                           2);
        break;
      case custom_browser::mojom::LayoutAlignment::kFill:
        adjusted.set_height(available_bounds.height());
        break;
    }
  }

  return adjusted;
}

const std::optional<std::string>& NativeUI::extra_style() const {
  return layout_params_->extra_style;
}

void NativeUI::Prepare() {
  OnPrepare();
}

void NativeUI::Layout() {
  OnLayout();
}

CustomBrowserView* NativeUI::GetCustomBrowserView() const {
  return static_cast<CustomBrowserView*>(host_view_->parent());
}

const std::vector<raw_ptr<views::View>>& NativeUI::views() const {
  return views_;
}

views::View* NativeUI::view() const {
  return views_.size() == 1 ? views_[0].get() : nullptr;
}

views::View* NativeUI::host_view() const {
  return host_view_.get();
}

views::View* NativeUI::GetViewAtIndex(size_t index) const {
  DCHECK(index < views_.size() && index >= 0);
  if (index >= views_.size()) {
    return nullptr;
  }
  return views_[index].get();
}

void NativeUI::OnPrepare() {}

bool NativeUI::visible() const {
  if (!layout_params_->visible || !host_view()->GetVisible()) {
    return false;
  }
  return true;
}

int NativeUI::hash_id() const {
  return layout_params_->hash_id;
}

int NativeUI::AddView(views::View* view) {
  DCHECK(view);
  views_.push_back(view);
  return static_cast<int>(views_.size() - 1);
}

void NativeUI::UpdateViewsVisibility() {
  for (const auto& view : views_) {
    if (view) {
      view->SetVisible(visible());
    }
  }
}

ViewNativeUI::ViewNativeUI(views::View* host_view,
                           const NativeViewParams* layout_params,
                           views::View* view)
    : NativeUI(host_view, layout_params) {
  DCHECK(view);
  views_.push_back(view);
}

ViewNativeUI::~ViewNativeUI() = default;

void ViewNativeUI::OnPrepare() {
  host_view()->AddChildView(view());
}

void ViewNativeUI::OnLayout() {
  if (!view()) {
    return;
  }
  UpdateViewsVisibility();
  if (visible()) {
    view()->SetBoundsRect(bounds_);
  }
}

DynamicViewNativeUI::DynamicViewNativeUI(views::View* host_view,
                                         const NativeViewParams* layout_params,
                                         views::View* view)
    : ViewNativeUI(host_view, layout_params, view) {
  utils::StartObservingViewLayout(this, view);
}

DynamicViewNativeUI::~DynamicViewNativeUI() {
  utils::StopObservingViewLayout(view());
}

NoopNativeUI::NoopNativeUI(views::View* host_view,
                           const NativeViewParams* layout_params)
    : NativeUI(host_view, layout_params) {
  LOG(ERROR) << "NoopNativeUI created for layout "
             << "class = "
             << layout_params->view_class_name.value_or("<unnamed>")
             << " id = " << layout_params->view_id.value_or("<no-id>");
}

NoopNativeUI::~NoopNativeUI() = default;

void NoopNativeUI::OnLayout() {
  // No-op layout
}

}  // namespace custom_browser::native_ui
