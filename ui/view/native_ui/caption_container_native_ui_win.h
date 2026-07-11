#ifndef CUSTOM_BROWSER_UI_VIEW_CAPTION_CONTAINER_NATIVE_UI_WIN_H_
#define CUSTOM_BROWSER_UI_VIEW_CAPTION_CONTAINER_NATIVE_UI_WIN_H_

#include "base/memory/raw_ptr.h"
#include "base/scoped_observation.h"
#include "chrome/browser/ui/views/frame/browser_caption_button_container_win.h"
#include "custom_browser/ui/view/native_ui/native_ui.h"
#include "custom_browser/ui/view/native_ui/native_ui_registration_macros.h"
#include "custom_browser/ui/view/native_ui/scoped_view_state.h"
#include "ui/views/view_observer.h"

class TabStripRegionView;
class TabStripActionContainer;
namespace views {
class Separator;
}  // namespace views

// Handles the browser caption button container on Windows. but its owner (the frame
// view) may be torn down whenever the native frame restarts. This class keeps
// the re find view alive while observing the frame so that the container is
// destroyed when the frame goes away.
namespace custom_browser::native_ui {

class CaptionContainerNativeUI : public ViewNativeUI,
                                 public views::ViewObserver {
 public:
  DECLARE_NATIVE_UI_CLASS_NAME(CaptionContainerNativeUI,
                               BrowserCaptionButtonContainer::kViewClassName)

  CaptionContainerNativeUI(
      views::View* host_view,
      const custom_browser::mojom::NativeViewParams* layout_params,
      views::View* view);
  ~CaptionContainerNativeUI() override;
  void OnPrepare() override;
  void OnLayout() override;

 private:
  // views::ViewObserver:
  void OnViewIsDeleting(views::View* observed_view) override;

  void InitializeCaptionButtonContainer(
      BrowserCaptionButtonContainer* container);
  BrowserCaptionButtonContainer* FindCaptionButtonContainer() const;
  void AttachTabStripActionContainer();

  base::ScopedObservation<views::View, views::ViewObserver>
      view_observation_{this};
  ScopedViewState<BrowserCaptionButtonContainer> caption_button_container_;
  // Reparented TabStripActionContainer tracked with automatic state restore.
  // ScopedViewState<TabStripActionContainer> tab_strip_action_container_;
};

}  // namespace custom_browser::native_ui

#endif  // CUSTOM_BROWSER_UI_VIEW_CAPTION_CONTAINER_NATIVE_UI_WIN_H_
