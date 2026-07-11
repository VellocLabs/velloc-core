#ifndef CUSTOM_BROWSER_UI_WEBUI_NATIVE_VIEW_HANDLER_H_
#define CUSTOM_BROWSER_UI_WEBUI_NATIVE_VIEW_HANDLER_H_

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "custom_browser/ui/mojom/ui_view_controller.mojom.h"
#include "custom_browser/ui/prefs/custom_browser_ui_prefs.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "ui/gfx/geometry/rect.h"

class CustomBrowserView;
class NativeUIContainerView;
class CustomBrowserUiPrefs;

// Handler for native view operations, enabling UI layout management from
// JavaScript
class UIViewHandler : public custom_browser::mojom::UIViewHandler,
                      public CustomBrowserUiPrefs::Observer {
 public:
  UIViewHandler(
      mojo::PendingReceiver<custom_browser::mojom::UIViewHandler> receiver,
      mojo::PendingRemote<custom_browser::mojom::UIView> native_view,
      CustomBrowserView* custom_browser_view);

  UIViewHandler(const UIViewHandler&) = delete;
  UIViewHandler& operator=(const UIViewHandler&) = delete;
  ~UIViewHandler() override;

  // custom_browser::mojom::UIViewHandler:
  void UpsertNativeViewLayouts(
      std::vector<custom_browser::mojom::NativeViewParamsPtr> batch) override;
  void SetUIMode(custom_browser::mojom::UIMode ui_mode) override;
  void GetUIMode(GetUIModeCallback callback) override;
  void OpenBrowserWindow() override;
  void SaveIntPref(custom_browser::mojom::IntKey key, int32_t value) override;
  void SaveBoolPref(custom_browser::mojom::BoolKey key, bool value) override;
  void EnableLayoutsSaving(bool enable) override;
  void StartCapture(const gfx::Rect& bound,
                    int32_t id,
                    int32_t delay_ms) override;

  // CustomBrowserUiPrefs::Observer
  void OnPrefUIModeChanged(custom_browser::mojom::UIMode ui_mode) override;

 private:
  void OnCaptureFinished(int id, bool success);

  mojo::Receiver<custom_browser::mojom::UIViewHandler> receiver_;
  mojo::Remote<custom_browser::mojom::UIView> native_view_;

  const raw_ptr<CustomBrowserView> custom_browser_view_;
  const raw_ptr<NativeUIContainerView> native_ui_container_;
  raw_ptr<CustomBrowserUiPrefs> ui_prefs_ = nullptr;
  base::WeakPtrFactory<UIViewHandler> weak_ptr_factory_{this};
};

#endif  // CUSTOM_BROWSER_UI_WEBUI_NATIVE_VIEW_HANDLER_H_
