#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_UI_NEXUS_UI_BRIDGE_ADAPTER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_UI_NEXUS_UI_BRIDGE_ADAPTER_H_

#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "custom_browser/browser/nexus/api/ui/ui_api.gen.h"
#include "custom_browser/browser/nexus/bridge_response_callback.h"
#include "ui/shell_dialogs/select_file_dialog.h"

class Profile;

namespace blink {
class MessagePortDescriptor;
}

namespace gfx {
class Image;
}

namespace message_center {
class Notification;
class NotificationDelegate;
}  // namespace message_center

namespace nexus {

class NexusBridgeResponseDelegate;

// The branded Nexus product logo (IDR_PRODUCT_LOGO_64, which the GRDs resolve to
// chrome/app/theme/${branding_path_component}/product_logo_64.png — the Velloc
// "V" supplied by the chromium_overrides symlink theme). Used as the toast
// HEADER (small) icon for OS notifications surfaced via ui.showNotification,
// replacing message_center's default kProductIcon (the Chromium spiral). Exposed
// at namespace scope so the custom_browser_nexus_browser_unittests regression
// test can assert it resolves to a non-empty image.
gfx::Image GetNexusNotificationHeaderIcon();

// Builds the toast surfaced via ui.showNotification (e.g. the turn-end "your
// turn" nudge). The toast HEADER text (message_center display_source — what
// Windows renders as "<name> • now") and the bold-line fallback (used when the
// guest supplies no session title) are both the branded product name from
// branding_constants.h, never the internal "Nexus" codename. Exposed at
// namespace scope so the custom_browser_nexus_browser_unittests regression
// test can assert that branding.
message_center::Notification BuildNexusToastNotification(
    const std::optional<std::string>& title,
    const std::string& message,
    const std::optional<std::string>& notification_type,
    scoped_refptr<message_center::NotificationDelegate> delegate);

// Writes `text` to the copy/paste clipboard of the current thread and reads it
// back: `done(true)` only when the clipboard holds exactly `text` afterwards.
// On Windows another process holding the clipboard makes the write a silent
// no-op, so the read-back is what tells ui.writeClipboard's caller to fall
// back to the page's clipboard paths. Exposed for the unit test.
void WriteNexusClipboardText(const std::string& text,
                             base::OnceCallback<void(bool)> done);

// Concrete adapter implementing the generated `UiApiBase` virtuals.
//
// Phase 11h architecture (mirrors api/llm — UI thread + typed inputs +
// BridgeResponseCallback):
//   - 11 actions declared under api/declarations/ui/*.api.json. All run
//     on the bridge's home thread (UI) — they touch BrowserView / WebUI
//     directly which is UI-bound.
//   - The codegen-emitted dispatcher extracts a typed input struct
//     and constructs `BridgeResponseCallback done`. The implementation
//     does its UI work and fires `done.Success/Error/InvalidArgs`.
//   - The two `broadcast*` actions use `passthrough_payload` because
//     the payload shape is opaque — they re-broadcast it verbatim as
//     a WebUI listener event.
//
// The `nexus_ui_broadcast.{h,cc}` helper that lived under
// `nexus/ui/` was deleted after the WebUI bridge migration retired
// its sole callers (currentSessionState / summaryChanged /
// dismissBootWelcome / showMainContents) — those events now ride
// direct worker → port broadcasts instead of profile-wide C++
// re-fanning. No remaining caller of
// `BroadcastWebUiListenerEventToProfile` exists.
class NexusUiBridgeAdapter : public ::nexus::generated::UiApiBase,
                             public ui::SelectFileDialog::Listener {
 public:
  NexusUiBridgeAdapter(NexusBridgeResponseDelegate* delegate, Profile* profile);
  ~NexusUiBridgeAdapter() override;

  NexusUiBridgeAdapter(const NexusUiBridgeAdapter&) = delete;
  NexusUiBridgeAdapter& operator=(const NexusUiBridgeAdapter&) = delete;

 private:
  // Overrides of generated::UiApiBase pure virtuals.

  void HandleOpenNexusSettingsOnUI(
      ::nexus::generated::OpenNexusSettingsInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleGetProviderMissingPromptOnUI(
      ::nexus::generated::GetProviderMissingPromptInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  // HandleDismissBootWelcomeOnUI + HandleShowMainContentsOnUI were
  // pure C++ pass-through relays — they received guest direct-bridge
  // calls and re-fired `nexusDismissBootWelcome` /
  // `nexusShowMainContents` via BroadcastWebUiListenerEventToProfile.
  // Retired alongside the `nexusCurrentSessionState` /
  // `nexusSessionsSummary` migration: the SharedWorker now fans the
  // same events directly to attached WebUI bridge ports via
  // `broadcastWebUiBridgeEvent`. Guest triggers via a new
  // `ui.broadcastEvent` SharedWorker port message; panel pages
  // subscribe via `nexus_ui_event_client.ts`.
  void HandleOpenUrlOnUI(
      ::nexus::generated::OpenUrlInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleOpenBrowserWindowOnUI(
      ::nexus::generated::OpenBrowserWindowInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleOpenLanguageSettingsOnUI(
      ::nexus::generated::OpenLanguageSettingsInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleWriteClipboardOnUI(
      ::nexus::generated::WriteClipboardInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleSetActiveSessionTitleOnUI(
      ::nexus::generated::SetActiveSessionTitleInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleSetWindowDragRegionsOnUI(
      ::nexus::generated::SetWindowDragRegionsInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleGetBootTimingsOnUI(
      ::nexus::generated::GetBootTimingsInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  // Read/write the `nexus.inspect_menu_enabled` profile pref — whether the
  // Nexus guest <webview>'s right-click menu offers "Inspect" (DevTools).
  // Backs the guest's hidden `/debug inspect [on|off]` slash subcommand.
  void HandleGetInspectMenuEnabledOnUI(
      ::nexus::generated::GetInspectMenuEnabledInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleSetInspectMenuEnabledOnUI(
      ::nexus::generated::SetInspectMenuEnabledInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  // HandleBroadcastCurrentSessionStateOnUI +
  // HandleBroadcastSessionsSummaryOnUI were pure pass-through relays:
  // C++ received a direct-bridge call from the SharedWorker and re-
  // fired it as `nexusCurrentSessionState` / `nexusSessionsSummary`
  // via BroadcastWebUiListenerEventToProfile. Both retired — the
  // worker now broadcasts the same payload directly to attached WebUI
  // bridge ports (one hop instead of three). See
  // `nexus_session_state_client.ts` for the panel-side subscriber.
  void HandleShowNotificationOnUI(
      ::nexus::generated::ShowNotificationInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandlePickProjectFolderOnUI(
      ::nexus::generated::PickProjectFolderInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandlePickAttachmentFilesOnUI(
      ::nexus::generated::PickAttachmentFilesInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleRevealProjectFolderOnUI(
      ::nexus::generated::RevealProjectFolderInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  // ui::SelectFileDialog::Listener — a native picker resolves asynchronously
  // into exactly one of these per SelectFile() call. `FileSelected` completes a
  // single-select (folder) pick; `MultiFilesSelected` completes a multi-select
  // (attachment files) pick; `FileSelectionCanceled` is shared and routes to
  // whichever request is in flight (only one dialog is ever open at a time).
  void FileSelected(const ui::SelectedFileInfo& file, int index) override;
  void MultiFilesSelected(
      const std::vector<ui::SelectedFileInfo>& files) override;
  void FileSelectionCanceled() override;

  raw_ptr<Profile> profile_;

  // Native folder-picker state for `ui.pickProjectFolder`. Single-in-flight:
  // a non-null `select_folder_dialog_` is the "a dialog is already open" guard;
  // `pending_pick_done_` holds the bridge response until a listener callback
  // above fires (or is dropped if we're destroyed first — see the dtor).
  scoped_refptr<ui::SelectFileDialog> select_folder_dialog_;
  ::nexus::BridgeResponseCallback pending_pick_done_;

  // Native multi-file-picker state for `ui.pickAttachmentFiles` (the composer's
  // paperclip button). Independent single-in-flight slot mirroring the folder
  // picker above; resolves from `MultiFilesSelected` / `FileSelectionCanceled`.
  scoped_refptr<ui::SelectFileDialog> select_files_dialog_;
  ::nexus::BridgeResponseCallback pending_files_done_;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_UI_NEXUS_UI_BRIDGE_ADAPTER_H_
