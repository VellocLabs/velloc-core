#ifndef CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_H_
#define CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_H_

#include <memory>
#include <optional>

#include "base/memory/raw_ptr.h"
#include "ui/gfx/geometry/rect.h"

namespace custom_browser::mojom {
class NativeViewParams;
}  // namespace custom_browser::mojom
using custom_browser::mojom::NativeViewParams;

class CustomBrowserView;
namespace views {
class View;
class SizeBounds;
}  // namespace views

namespace custom_browser::native_ui {

class NativeUI {
 public:
  NativeUI(views::View* host_view, const NativeViewParams* layout_params);
  virtual ~NativeUI();

  NativeUI(const NativeUI&) = delete;
  NativeUI& operator=(const NativeUI&) = delete;

  void Update(const NativeViewParams* layout_params);

  const NativeViewParams* layout_params() const { return layout_params_; }

  const gfx::Rect& bounds() const { return bounds_; }

  void CalculateBounds();
  void Prepare();
  void Layout();
  CustomBrowserView* GetCustomBrowserView() const;
  views::View* GetViewAtIndex(size_t index) const;
  const std::vector<raw_ptr<views::View>>& views() const;
  const std::optional<std::string>& extra_style() const;
  views::View* view() const;
  views::View* host_view() const;
  bool visible() const;
  int hash_id() const;

  views::View* FindViewById(int id) const;
  views::View* FindViewByClass(const std::string& class_name) const;

 protected:
  virtual void OnPrepare();
  virtual void OnLayout() = 0;
#if DCHECK_IS_ON()
  virtual bool LogInfo() const;
#endif
  virtual gfx::Size GetPreferredSize(
      const views::SizeBounds& size_bounds) const;
  void UpdateViewsVisibility();
  int AddView(views::View* view);
  gfx::Rect CalculateAlignmentBounds(const gfx::Rect& available_bounds) const;

  raw_ptr<views::View> host_view_;
  raw_ptr<const NativeViewParams> layout_params_;
  std::vector<raw_ptr<views::View>> views_;
  gfx::Rect bounds_;
};

class ViewNativeUI : public NativeUI {
 public:
  ViewNativeUI(views::View* host_view,
               const NativeViewParams* layout_params,
               views::View* view);
  ~ViewNativeUI() override;

 protected:
  void OnPrepare() override;
  void OnLayout() override;
};

class DynamicViewNativeUI : public ViewNativeUI {
 public:
  DynamicViewNativeUI(views::View* host_view,
                      const NativeViewParams* layout_params,
                      views::View* view);
  ~DynamicViewNativeUI() override;
};

class NoopNativeUI : public NativeUI {
 public:
  NoopNativeUI(views::View* host_view, const NativeViewParams* layout_params);
  ~NoopNativeUI() override;

 protected:
  void OnLayout() override;
};

}  // namespace custom_browser::native_ui

#endif  // CUSTOM_BROWSER_UI_VIEW_NATIVE_UI_H_
