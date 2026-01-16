#include "custom_browser/ui/web_ui/native_view_handler.h"

#include "base/functional/bind.h"
#include "base/time/time.h"
#include "base/trace_event/trace_event.h"
#include "custom_browser/ui/prefs/custom_browser_ui_prefs.h"
#include "custom_browser/ui/view/custom_browser_view.h"

UIViewHandler::UIViewHandler(
    mojo::PendingReceiver<custom_browser::mojom::UIViewHandler> receiver,
    mojo::PendingRemote<custom_browser::mojom::UIView> native_view,
    CustomBrowserView* custom_browser_view)
    : receiver_(this, std::move(receiver)),
      native_view_(std::move(native_view)),
      custom_browser_view_(custom_browser_view),
      native_ui_container_(custom_browser_view->native_ui_container()),
      ui_prefs_(custom_browser_view->ui_prefs()) {
  DCHECK(native_ui_container_);
  DCHECK(native_view_ && native_view_.is_bound());
  if (ui_prefs_) {
    ui_prefs_->AddObserver(this);
  }
}

UIViewHandler::~UIViewHandler() {
  if (ui_prefs_) {
    ui_prefs_->RemoveObserver(this);
  }
}

void UIViewHandler::UpsertNativeViewLayouts(
    std::vector<custom_browser::mojom::NativeViewParamsPtr> batch) {
  TRACE_EVENT0("browser", "UIViewHandler::UpsertNativeViewLayouts");
  // Persist batch via CustomBrowserView-owned prefs.
  if (custom_browser_view_ && custom_browser_view_->ui_prefs()) {
    custom_browser_view_->ui_prefs()->UpdateLayouts(std::move(batch));
  }
}

void UIViewHandler::SetUIMode(custom_browser::mojom::UIMode ui_mode) {
  TRACE_EVENT0("browser", "UIViewHandler::SetUIMode");
  if (custom_browser_view_ && custom_browser_view_->ui_prefs()) {
    custom_browser_view_->ui_prefs()->UpdateUIMode(ui_mode);
  }
}

void UIViewHandler::SaveIntPref(custom_browser::mojom::IntKey key,
                                int32_t value) {
  TRACE_EVENT0("browser", "UIViewHandler::SaveIntPref");
  if (ui_prefs_) {
    ui_prefs_->SaveIntPref(key, value);
  }
}

void UIViewHandler::SaveBoolPref(custom_browser::mojom::BoolKey key,
                                 bool value) {
  TRACE_EVENT0("browser", "UIViewHandler::SaveBoolPref");
  if (ui_prefs_) {
    ui_prefs_->SaveBoolPref(key, value);
  }
}

void UIViewHandler::EnableLayoutsSaving(bool enable) {
  TRACE_EVENT0("browser", "UIViewHandler::EnableLayoutsSaving");
  if (ui_prefs_) {
    ui_prefs_->EnableLayoutsSaving(enable);
  }
}

void UIViewHandler::StartCapture(const gfx::Rect& bound,
                                 int32_t id,
                                 int32_t delay_ms) {
  if (!custom_browser_view_) {
    OnCaptureFinished(id, /*success=*/false);
    return;
  }

  const base::TimeDelta delay =
      delay_ms > 0 ? base::Milliseconds(delay_ms) : base::TimeDelta();

  auto completion = base::BindOnce(&UIViewHandler::OnCaptureFinished,
                                   weak_ptr_factory_.GetWeakPtr(), id);
  custom_browser_view_->StartMouseCapture(bound, id, delay,
                                          std::move(completion));
}

void UIViewHandler::OnPrefUIModeChanged(custom_browser::mojom::UIMode ui_mode) {
  if (!native_view_.is_bound()) {
    return;
  }
  native_view_->PrefUIModeChanged(ui_mode);
}

void UIViewHandler::OnCaptureFinished(int id, bool success) {
  // |success| is currently informational; WebUI clients only need the id.
  (void)success;
  if (!native_view_.is_bound()) {
    return;
  }
  native_view_->CaptureCompleted(id);
}
