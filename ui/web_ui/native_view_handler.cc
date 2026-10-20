#include "custom_browser/ui/web_ui/native_view_handler.h"

#include "base/functional/bind.h"
#include "base/time/time.h"
#include "base/trace_event/trace_event.h"
#include "chrome/browser/ui/browser.h"
#include "custom_browser/ui/prefs/custom_browser_ui_prefs.h"
#include "custom_browser/ui/view/custom_browser_view.h"
#include "custom_browser/ui/view/custom_browser_window_util.h"

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
    // Note: the WebUI pulls its authoritative per-window mode via GetUIMode()
    // on startup (see ui_store.ts), so there is no fire-and-forget push here.
  }
}

UIViewHandler::~UIViewHandler() {
  if (ui_prefs_) {
    ui_prefs_->RemoveObserver(this);
  }
}

void UIViewHandler::DetachFromView() {
  if (ui_prefs_) {
    ui_prefs_->RemoveObserver(this);
  }
  ui_prefs_ = nullptr;
  native_ui_container_ = nullptr;
  custom_browser_view_ = nullptr;
  receiver_.Pause();
}

void UIViewHandler::AttachToView(CustomBrowserView* custom_browser_view) {
  DCHECK(custom_browser_view);
  DCHECK(!custom_browser_view_) << "AttachToView without DetachFromView";
  custom_browser_view_ = custom_browser_view;
  native_ui_container_ = custom_browser_view->native_ui_container();
  ui_prefs_ = custom_browser_view->ui_prefs();
  if (ui_prefs_) {
    ui_prefs_->AddObserver(this);
  }
  receiver_.Resume();
  // The page pulled its mode once at load (GetUIMode); a re-attached page
  // needs the new window's authoritative mode pushed instead.
  if (ui_prefs_ && native_view_.is_bound()) {
    native_view_->PrefUIModeChanged(ui_prefs_->current_mode());
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

void UIViewHandler::GetUIMode(GetUIModeCallback callback) {
  TRACE_EVENT0("browser", "UIViewHandler::GetUIMode");
  // Authoritative per-window mode (kNexus for the nexus window, the shared
  // browser mode otherwise). Lets the WebUI sync on startup without relying on
  // the data source's profile-global baked value.
  std::move(callback).Run(ui_prefs_ ? ui_prefs_->current_mode()
                                     : custom_browser::mojom::UIMode::kSidebar);
}

void UIViewHandler::OpenBrowserWindow() {
  TRACE_EVENT0("browser", "UIViewHandler::OpenBrowserWindow");
  if (!custom_browser_view_ || !custom_browser_view_->browser()) {
    return;
  }
  // Do NOT change this (pinned nexus) window's UI mode. Focus an existing
  // normal browser window for the profile, or open a new one.
  custom_browser::window_util::OpenOrFocusNormalWindow(
      custom_browser_view_->browser()->GetProfile());
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
