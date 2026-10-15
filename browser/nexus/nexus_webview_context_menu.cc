#include "custom_browser/browser/nexus/nexus_webview_context_menu.h"

#include <memory>
#include <string>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/single_thread_task_runner.h"
#include "chrome/browser/devtools/devtools_window.h"
#include "chrome/browser/profiles/profile.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/context_menu_params.h"
#include "content/public/browser/global_routing_id.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "custom_browser/browser/nexus/api/llm/llm_pref_names.h"
#include "custom_browser/browser/nexus/panel/nexus_panel_guest.h"
#include "extensions/browser/guest_view/web_view/web_view_guest.h"
#include "ui/aura/client/screen_position_client.h"
#include "ui/aura/window.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/menus/simple_menu_model.h"
#include "ui/views/controls/menu/menu_runner.h"
#include "ui/views/widget/widget.h"

namespace nexus {

namespace {

constexpr int kInspectCommandId = 1;

// Self-owning single-item "Inspect" context menu. Created by
// `MaybeHandleNexusGuestContextMenu` when the pref is on; deletes itself
// when the menu closes (after any command has executed). The frame is held
// as a GlobalRenderFrameHostId and re-resolved at execute time so a guest
// navigation / teardown while the menu is open degrades to a no-op instead
// of a use-after-free.
class NexusInspectMenu : public ui::SimpleMenuModel::Delegate {
 public:
  static void Show(content::WebContents* guest_contents,
                   content::RenderFrameHost& render_frame_host,
                   const content::ContextMenuParams& params) {
    // Owns itself; deleted via `OnMenuClosed` → `DeleteSoon`.
    new NexusInspectMenu(guest_contents, render_frame_host, params);
  }

  NexusInspectMenu(const NexusInspectMenu&) = delete;
  NexusInspectMenu& operator=(const NexusInspectMenu&) = delete;
  ~NexusInspectMenu() override = default;

 private:
  NexusInspectMenu(content::WebContents* guest_contents,
                   content::RenderFrameHost& render_frame_host,
                   const content::ContextMenuParams& params)
      : frame_id_(render_frame_host.GetGlobalId()),
        x_(params.x),
        y_(params.y),
        model_(this) {
    // Hardcoded English, matching the fork's other native-menu strings —
    // this is a developer-facing item behind `/debug inspect on`.
    model_.AddItem(kInspectCommandId, u"Inspect");
    menu_runner_ = std::make_unique<views::MenuRunner>(
        &model_, views::MenuRunner::CONTEXT_MENU,
        base::BindRepeating(&NexusInspectMenu::OnMenuClosed,
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
  void ExecuteCommand(int command_id, int /*event_flags*/) override {
    if (command_id != kInspectCommandId) {
      return;
    }
    // Re-resolve — the guest may have navigated or gone away while the
    // menu was open. InspectElement opens (or focuses) DevTools on the
    // guest and inspects the node at the original click point.
    if (content::RenderFrameHost* render_frame_host =
            content::RenderFrameHost::FromID(frame_id_)) {
      DevToolsWindow::InspectElement(render_frame_host, x_, y_);
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

  // Pref off (or unreachable): swallow the event — the panel shows no
  // context menu. This is the default product behavior.
  if (!pref_service ||
      !pref_service->GetBoolean(prefs::kNexusInspectMenuEnabled)) {
    return true;
  }

  NexusInspectMenu::Show(guest_contents, render_frame_host, params);
  return true;
}

}  // namespace nexus
