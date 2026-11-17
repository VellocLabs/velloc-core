#include "custom_browser/browser/nexus/nexus_webview_context_menu.h"

#include <memory>
#include <vector>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/notreached.h"
#include "base/task/single_thread_task_runner.h"
#include "chrome/browser/devtools/devtools_window.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/grit/generated_resources.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/context_menu_params.h"
#include "content/public/browser/global_routing_id.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "custom_browser/browser/nexus/api/llm/llm_pref_names.h"
#include "custom_browser/browser/nexus/nexus_guest_menu_model.h"
#include "custom_browser/browser/nexus/panel/nexus_panel_guest.h"
#include "extensions/browser/guest_view/web_view/web_view_guest.h"
#include "ui/aura/client/screen_position_client.h"
#include "ui/aura/window.h"
#include "ui/base/accelerators/accelerator.h"
#include "ui/events/event_constants.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/menus/simple_menu_model.h"
#include "ui/views/controls/menu/menu_runner.h"
#include "ui/views/widget/widget.h"

namespace nexus {

namespace {

int LabelIdFor(GuestMenuCommand command) {
  switch (command) {
    case GuestMenuCommand::kUndo:
      return IDS_CONTENT_CONTEXT_UNDO;
    case GuestMenuCommand::kRedo:
      return IDS_CONTENT_CONTEXT_REDO;
    case GuestMenuCommand::kCut:
      return IDS_CONTENT_CONTEXT_CUT;
    case GuestMenuCommand::kCopy:
      return IDS_CONTENT_CONTEXT_COPY;
    case GuestMenuCommand::kPaste:
      return IDS_CONTENT_CONTEXT_PASTE;
    case GuestMenuCommand::kSelectAll:
      return IDS_CONTENT_CONTEXT_SELECTALL;
    case GuestMenuCommand::kInspect:
      return IDS_CONTENT_CONTEXT_INSPECTELEMENT;
    case GuestMenuCommand::kSeparator:
      break;
  }
  NOTREACHED();
}

// Self-owning guest context menu built from `BuildGuestMenuItems`; deletes
// itself when the menu closes (after any command has executed). The frame
// is held as a GlobalRenderFrameHostId and re-resolved at execute time so a
// guest navigation / teardown while the menu is open degrades to a no-op
// instead of a use-after-free.
class NexusGuestMenu : public ui::SimpleMenuModel::Delegate {
 public:
  static void Show(content::WebContents* guest_contents,
                   content::RenderFrameHost& render_frame_host,
                   const content::ContextMenuParams& params,
                   const std::vector<GuestMenuItem>& items) {
    // Owns itself; deleted via `OnMenuClosed` → `DeleteSoon`.
    new NexusGuestMenu(guest_contents, render_frame_host, params, items);
  }

  NexusGuestMenu(const NexusGuestMenu&) = delete;
  NexusGuestMenu& operator=(const NexusGuestMenu&) = delete;
  ~NexusGuestMenu() override = default;

 private:
  NexusGuestMenu(content::WebContents* guest_contents,
                 content::RenderFrameHost& render_frame_host,
                 const content::ContextMenuParams& params,
                 const std::vector<GuestMenuItem>& items)
      : frame_id_(render_frame_host.GetGlobalId()),
        x_(params.x),
        y_(params.y),
        model_(this) {
    for (const GuestMenuItem& item : items) {
      if (item.command == GuestMenuCommand::kSeparator) {
        model_.AddSeparator(ui::NORMAL_SEPARATOR);
        continue;
      }
      model_.AddItemWithStringId(static_cast<int>(item.command),
                                 LabelIdFor(item.command));
      model_.SetEnabledAt(model_.GetItemCount() - 1, item.enabled);
    }
    menu_runner_ = std::make_unique<views::MenuRunner>(
        &model_,
        views::MenuRunner::HAS_MNEMONICS | views::MenuRunner::CONTEXT_MENU,
        base::BindRepeating(&NexusGuestMenu::OnMenuClosed,
                            base::Unretained(this)));

    // Convert the guest-view-relative click point to screen coordinates —
    // the same aura conversion RenderViewContextMenuViews::Show performs.
    gfx::Point screen_point(params.x, params.y);
    views::Widget* top_level_widget = nullptr;
    aura::Window* target_window =
        guest_contents ? guest_contents->GetNativeView() : nullptr;
    if (target_window) {
      if (aura::Window* root_window = target_window->GetRootWindow()) {
        if (auto* screen_position_client =
                aura::client::GetScreenPositionClient(root_window)) {
          screen_position_client->ConvertPointToScreen(target_window,
                                                       &screen_point);
        }
      }
      top_level_widget =
          views::Widget::GetTopLevelWidgetForNativeView(target_window);
    }
    if (!top_level_widget) {
      // No widget to anchor to (window mid-teardown) — nothing to show.
      OnMenuClosed();
      return;
    }
    menu_runner_->RunMenuAt(top_level_widget, /*button_controller=*/nullptr,
                            gfx::Rect(screen_point, gfx::Size()),
                            views::MenuAnchorPosition::kTopLeft,
                            params.source_type);
  }

  // ui::SimpleMenuModel::Delegate:
  bool GetAcceleratorForCommandId(int command_id,
                                  ui::Accelerator* accelerator) const override {
    ui::KeyboardCode key = ui::VKEY_UNKNOWN;
    switch (static_cast<GuestMenuCommand>(command_id)) {
      case GuestMenuCommand::kUndo:
        key = ui::VKEY_Z;
        break;
      case GuestMenuCommand::kRedo:
        key = ui::VKEY_Y;
        break;
      case GuestMenuCommand::kCut:
        key = ui::VKEY_X;
        break;
      case GuestMenuCommand::kCopy:
        key = ui::VKEY_C;
        break;
      case GuestMenuCommand::kPaste:
        key = ui::VKEY_V;
        break;
      case GuestMenuCommand::kSelectAll:
        key = ui::VKEY_A;
        break;
      case GuestMenuCommand::kInspect:
      case GuestMenuCommand::kSeparator:
        return false;
    }
    *accelerator = ui::Accelerator(key, ui::EF_CONTROL_DOWN);
    return true;
  }

  void ExecuteCommand(int command_id, int /*event_flags*/) override {
    // Re-resolve — the guest may have navigated or gone away while the
    // menu was open.
    content::RenderFrameHost* render_frame_host =
        content::RenderFrameHost::FromID(frame_id_);
    if (!render_frame_host) {
      return;
    }
    content::WebContents* contents =
        content::WebContents::FromRenderFrameHost(render_frame_host);
    if (!contents) {
      return;
    }
    // Edit commands act on the guest's focused frame — the one the
    // right-click landed in.
    switch (static_cast<GuestMenuCommand>(command_id)) {
      case GuestMenuCommand::kUndo:
        contents->Undo();
        return;
      case GuestMenuCommand::kRedo:
        contents->Redo();
        return;
      case GuestMenuCommand::kCut:
        contents->Cut();
        return;
      case GuestMenuCommand::kCopy:
        contents->Copy();
        return;
      case GuestMenuCommand::kPaste:
        contents->Paste();
        return;
      case GuestMenuCommand::kSelectAll:
        contents->SelectAll();
        return;
      case GuestMenuCommand::kInspect:
        // Opens (or focuses) DevTools on the guest and inspects the node
        // at the original click point.
        DevToolsWindow::InspectElement(render_frame_host, x_, y_);
        return;
      case GuestMenuCommand::kSeparator:
        return;
    }
  }

  void OnMenuClosed() {
    // `ExecuteCommand` (if any) runs synchronously before the posted
    // deletion, so DeleteSoon is safe from both the closed callback and
    // the no-widget bail-out above.
    base::SingleThreadTaskRunner::GetCurrentDefault()->DeleteSoon(FROM_HERE,
                                                                  this);
  }

  const content::GlobalRenderFrameHostId frame_id_;
  const int x_;
  const int y_;
  ui::SimpleMenuModel model_;
  std::unique_ptr<views::MenuRunner> menu_runner_;
};

}  // namespace

bool MaybeHandleNexusGuestContextMenu(
    extensions::WebViewGuest* guest,
    content::RenderFrameHost& render_frame_host,
    const content::ContextMenuParams& params) {
  if (!IsNexusPanelGuest(guest, render_frame_host)) {
    return false;
  }

  content::WebContents* guest_contents =
      content::WebContents::FromRenderFrameHost(&render_frame_host);
  Profile* profile =
      guest_contents
          ? Profile::FromBrowserContext(guest_contents->GetBrowserContext())
          : nullptr;
  PrefService* pref_service = profile ? profile->GetPrefs() : nullptr;
  const bool inspect_enabled =
      pref_service && pref_service->GetBoolean(prefs::kNexusInspectMenuEnabled);

  // Always handled: an empty item list swallows the event, so the guest
  // never falls through to the default webview menu.
  const std::vector<GuestMenuItem> items = BuildGuestMenuItems(
      params.is_editable, params.edit_flags, inspect_enabled);
  if (!items.empty()) {
    NexusGuestMenu::Show(guest_contents, render_frame_host, params, items);
  }
  return true;
}

}  // namespace nexus
