#include "custom_browser/browser/nexus/api/ui/nexus_ui_bridge_adapter.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/notifications/notification_display_service.h"
#include "chrome/browser/notifications/notification_display_service_factory.h"
#include "chrome/browser/notifications/notification_handler.h"
#include "chrome/browser/platform_util.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/grit/chrome_unscaled_resources.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "custom_browser/ui/view/custom_browser_view.h"
#include "custom_browser/ui/view/custom_browser_window_util.h"
#include "custom_browser/ui/web_ui/custom_browser_webui_view.h"
#include "ui/base/clipboard/clipboard.h"
#include "ui/base/clipboard/clipboard_buffer.h"
#include "ui/base/clipboard/scoped_clipboard_writer.h"
#include "ui/base/models/image_model.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/image/canvas_image_source.h"
#include "ui/gfx/image/image.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/shell_dialogs/select_file_policy.h"
#include "ui/shell_dialogs/selected_file_info.h"
#include "ui/message_center/public/cpp/message_center_constants.h"
#include "ui/message_center/public/cpp/notification.h"
#include "ui/message_center/public/cpp/notification_delegate.h"
#include "ui/message_center/public/cpp/notification_types.h"
#include "ui/message_center/public/cpp/notifier_id.h"
#include "components/prefs/pref_service.h"
#include "custom_browser/browser/nexus/api/llm/llm_pref_names.h"
#include "custom_browser/common/branding_constants.h"
#include "url/gurl.h"

namespace nexus {

namespace {

bool IsValidSettingsSection(std::string_view section) {
  return section == "models" || section == "webapp" || section == "mcp" ||
         section == "features";
}

// Draws the branded product logo inset within an 18-dip badge so the toast
// header icon matches the visual footprint of message_center's default icon
// (a vector that leaves internal whitespace rather than filling its box). The
// product_logo PNG is a full-bleed rounded square, so without the inset it
// reads noticeably larger/heavier than the icon it replaces. Scale-aware
// (downscales the 64px source per display rep) so it stays crisp on HiDPI.
class NexusHeaderIconSource : public gfx::CanvasImageSource {
 public:
  NexusHeaderIconSource()
      : gfx::CanvasImageSource(gfx::Size(message_center::kSmallImageSizeMD,
                                         message_center::kSmallImageSizeMD)) {}
  NexusHeaderIconSource(const NexusHeaderIconSource&) = delete;
  NexusHeaderIconSource& operator=(const NexusHeaderIconSource&) = delete;
  ~NexusHeaderIconSource() override = default;

  void Draw(gfx::Canvas* canvas) override {
    // Logo occupies `kInner` of the `kBox`-dip badge; the remainder is
    // transparent margin. Tune `kInner` to grow/shrink the badge.
    constexpr int kBox = message_center::kSmallImageSizeMD;  // 18
    constexpr int kInner = 14;
    constexpr int kMargin = (kBox - kInner) / 2;
    const gfx::ImageSkia logo =
        *ui::ResourceBundle::GetSharedInstance().GetImageSkiaNamed(
            IDR_PRODUCT_LOGO_64);
    canvas->DrawImageInt(logo, /*src_x=*/0, /*src_y=*/0, logo.width(),
                         logo.height(), /*dest_x=*/kMargin, /*dest_y=*/kMargin,
                         /*dest_w=*/kInner, /*dest_h=*/kInner, /*filter=*/true);
  }
};

// Length cap (UTF-16 units) for the session title shown in the native window
// title. Taskbar/alt-tab elide on their own; this is a sanity guard against
// hostile prompt-derived titles.
constexpr size_t kMaxNexusWindowTitleLength = 120;

bool OpenNexusSettingsForProfile(Profile* profile, std::string_view section) {
  if (!profile) {
    return false;
  }

  bool opened = false;
  ForEachCurrentBrowserWindowInterfaceOrderedByActivation(
      [&](BrowserWindowInterface* window) {
        Browser* browser =
            window ? window->GetBrowserForMigrationOnly() : nullptr;
        if (!browser || browser->profile() != profile ||
            !browser->is_type_normal()) {
          return true;
        }

        auto* browser_view = BrowserView::GetBrowserViewForBrowser(browser);
        if (!browser_view) {
          return true;
        }

        auto* custom_browser_view = static_cast<CustomBrowserView*>(browser_view);
        auto* webui_view =
            custom_browser_view->views().custom_browser_webui_view.get();
        content::WebContents* web_contents =
            webui_view ? webui_view->GetWebContents() : nullptr;
        content::WebUI* web_ui = web_contents ? web_contents->GetWebUI() : nullptr;
        if (!web_ui || !web_ui->CanCallJavascript()) {
          return true;
        }

        web_ui->CallJavascriptFunctionUnsafe(
            "customBrowserOpenNexusSettings",
            base::Value(std::string(section)));
        opened = true;
        return false;
      });

  return opened;
}

// Pushes the nexus topbar's window-drag map (reported by the guest) into the
// host's CustomBrowserWebUIView so the frameless window can be dragged from the
// React topbar. Targets the most-recently-active nexus window for the profile
// (the one the user is interacting with). NOTE: with multiple nexus windows the
// regions are window-specific (right-aligned controls differ by width), and the
// bridge adapter only knows the profile, not which window sent this — so only
// the active window stays exactly correct. Single-window (the common case) is
// always correct.
void ApplyGuestDragRegionForProfile(Profile* profile,
                                    const std::vector<gfx::Rect>& draggable,
                                    const std::vector<gfx::Rect>& subtract) {
  if (!profile) {
    return;
  }
  ForEachCurrentBrowserWindowInterfaceOrderedByActivation(
      [&](BrowserWindowInterface* window) {
        Browser* browser =
            window ? window->GetBrowserForMigrationOnly() : nullptr;
        if (!browser || browser->profile() != profile ||
            !browser->is_type_normal()) {
          return true;  // Keep scanning.
        }
        auto* browser_view = BrowserView::GetBrowserViewForBrowser(browser);
        if (!browser_view) {
          return true;
        }
        auto* custom_browser_view = static_cast<CustomBrowserView*>(browser_view);
        auto* webui_view =
            custom_browser_view->views().custom_browser_webui_view.get();
        if (!webui_view) {
          return true;
        }
        webui_view->SetGuestDragRegion(draggable, subtract);
        return false;  // Applied to the active nexus window; stop.
      });
}

// Returns true if a normal browser window for `profile` is currently the
// active (OS-focused) window. Used to suppress turn-end notifications while
// the user is actively looking at Nexus.
bool IsNexusBrowserWindowActiveForProfile(Profile* profile) {
  if (!profile) {
    return false;
  }

  bool active = false;
  ForEachCurrentBrowserWindowInterfaceOrderedByActivation(
      [&](BrowserWindowInterface* window) {
        Browser* browser =
            window ? window->GetBrowserForMigrationOnly() : nullptr;
        if (!browser || browser->profile() != profile ||
            !browser->is_type_normal()) {
          return true;  // Keep scanning.
        }
        auto* browser_view = BrowserView::GetBrowserViewForBrowser(browser);
        if (browser_view && browser_view->IsActive()) {
          active = true;
          return false;  // Found an active window; stop.
        }
        return true;
      });
  return active;
}

// Activates the normal browser window hosting Nexus for the profile
// identified by `profile_id`. Invoked when the user clicks a turn-end
// notification — which can happen long after it was posted, possibly after
// the originating Profile was destroyed (the OS may keep the toast around).
// So `profile_id` is an opaque identity token: it is only COMPARED against
// live profiles' addresses, never dereferenced. If nothing matches (profile
// gone, or all its windows closed) this is a no-op.
void ActivateNexusWindowForProfileId(uintptr_t profile_id) {
  ForEachCurrentBrowserWindowInterfaceOrderedByActivation(
      [&](BrowserWindowInterface* window) {
        Browser* browser =
            window ? window->GetBrowserForMigrationOnly() : nullptr;
        if (!browser || !browser->is_type_normal() ||
            reinterpret_cast<uintptr_t>(browser->profile()) != profile_id) {
          return true;  // Keep scanning.
        }
        if (auto* browser_view =
                BrowserView::GetBrowserViewForBrowser(browser)) {
          browser_view->Activate();
          return false;  // Brought the Nexus window forward; stop.
        }
        return true;
      });
}

// Returns the native window of the most-recently-active normal browser window
// hosting Nexus for `profile`, to parent the folder-picker dialog (so it is
// modal to the Nexus window). Returns a null window when none is found — the
// dialog then opens modeless, which is still functional.
gfx::NativeWindow GetNexusOwningWindowForProfile(Profile* profile) {
  gfx::NativeWindow owning_window = gfx::NativeWindow();
  if (!profile) {
    return owning_window;
  }
  ForEachCurrentBrowserWindowInterfaceOrderedByActivation(
      [&](BrowserWindowInterface* window) {
        Browser* browser =
            window ? window->GetBrowserForMigrationOnly() : nullptr;
        if (!browser || browser->profile() != profile ||
            !browser->is_type_normal()) {
          return true;  // Keep scanning.
        }
        if (BrowserWindow* browser_window = browser->window()) {
          owning_window = browser_window->GetNativeWindow();
          return false;  // Found the Nexus window; stop.
        }
        return true;
      });
  return owning_window;
}

}  // namespace

constexpr char kProviderMissingHeaderIconSvg[] =
    R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 128 128" width="128" height="128" aria-hidden="true" preserveAspectRatio="xMidYMid meet"><circle cx="62.5" cy="112" r="12" fill="#c33"/><circle cx="60.9" cy="112" r="10.3" fill="#f44336"/><path d="M55.1 108.4c1.2-1.8 3.8-3.3 6.5-3.7c.7-.1 1.3-.1 1.9.1c.4.2.8.6.5 1c-.2.4-.7.5-1.1.6c-2.5.7-4.8 2.4-6.2 4.4c-.5.8-1.4 2.9-2.4 2.4c-1-.7-.8-2.6.8-4.8z" opacity=".2" fill="#ffffff"/><path d="M46.1 24.3c-2.1 2-3.4 4.4-4 7.4c-.3 1.9-2 3.2-3.9 3.2h6.9c1.9 0 3.6-1.3 3.9-3.2c.5-3 1.9-5.4 4-7.4c2.1-1.9 4.7-3.1 7.8-3.6c-1.1-.2-2.3-.3-3.5-.3c-4.6.1-8.3 1.4-11.2 3.9z" fill="#c33"/><path d="M89.5 12.2C83.4 6.7 75 4 64.2 4c-1.2 0-2.3 0-3.4.1c9.1.5 16.4 3.2 21.8 8.1c6.1 5.5 9.1 13.2 9.1 23c0 8.8-4.1 17.4-12.2 25.9l-9.8 9.6l-.3.3c-2.9 3.4-4.6 8-5.2 13.9c-.2 2-1.9 3.6-4 3.6h6.9c2 0 3.8-1.6 4-3.6c.5-5.9 2.3-10.5 5.2-13.9l.3-.3l9.8-9.6c8.2-8.5 12.2-17.1 12.2-25.9c-.1-9.8-3.1-17.5-9.1-23z" fill="#c33"/><path d="M64.2 85c.5-5.9 2.3-10.5 5.2-13.9l.3-.3l9.8-9.6c8.2-8.5 12.2-17.1 12.2-25.9c0-9.8-3-17.5-9.1-23C77.2 7.4 70 4.7 60.8 4.2c-8.9.5-16.2 3.3-21.9 8.2c-5.3 4.6-8.5 10.6-9.4 18.1c-.3 2.4 1.6 4.4 4 4.4h4.8c1.9 0 3.6-1.3 3.9-3.2c.5-3 1.9-5.4 4-7.4c2.8-2.6 6.6-3.9 11.2-3.9c1.2 0 2.4.1 3.5.3c1.1-.2 2.2-.3 3.4-.3c4.8 0 8.5 1.3 11 4s3.8 6.5 3.8 11.5c0 3.8-1.1 7.4-3.2 10.6c-1.4 2.2-4.7 5.8-10 10.9c-5.2 5.1-8.8 9.8-10.5 14c-1.4 3.4-2.3 7.7-2.5 12.9c-.1 2.3 1.7 4.2 4 4.2h3.5c1.8.1 3.6-1.5 3.8-3.5z" fill="#f44336"/><path d="M39.6 15.9C43 11.7 49.5 8.7 56 8.3c1.6-.1 3.2 0 4.5.7c1 .5 1.7 1.6 1 2.6c-.6.8-1.8 1.1-2.8 1.3c-6.1 1.2-12 4.7-15.9 9.3c-1.5 1.8-4 6.8-6.2 5.2c-2.3-1.9-1.2-6.4 3-11.5z" opacity=".2" fill="#ffffff"/></svg>)SVG";

constexpr char kProviderMissingHeaderTitle[] = "No model provider";
constexpr char kProviderMissingHeaderCaption[] =
    "Open settings to configure";

constexpr char kProviderMissingDialogTitle[] = "Set up a model provider";
constexpr char kProviderMissingDialogDescription[] =
    "You need at least one configured and enabled model provider before "
    "sending a message. Your draft will be restored when this dialog closes.";
constexpr char kProviderMissingDialogConfirmLabel[] = "Open settings";
constexpr char kProviderMissingDialogCancelLabel[] = "Cancel";

NexusUiBridgeAdapter::NexusUiBridgeAdapter(
    NexusBridgeResponseDelegate* delegate,
    Profile* profile)
    : ::nexus::generated::UiApiBase(delegate), profile_(profile) {
  // Register(...) calls live in `UiApiBase`'s ctor, generated from
  // api/declarations/ui/*.api.json. Adding a new ui wire action: drop
  // a *.api.json under api/declarations/ui/, add a Handle*OnUI()
  // override here. The codegen wires the dispatch automatically.
}

NexusUiBridgeAdapter::~NexusUiBridgeAdapter() {
  // If a folder dialog is still open at teardown, detach as its listener so
  // it can't call back into freed memory (mirrors
  // DownloadsHandler::~DownloadsHandler). Any pending bridge response is then
  // dropped — the guest's promise settles via the bridge's disconnect path.
  if (select_folder_dialog_) {
    select_folder_dialog_->ListenerDestroyed();
  }
  if (select_files_dialog_) {
    select_files_dialog_->ListenerDestroyed();
  }
}

// ===========================================================================
// Per-action handlers — all run on UI (the bridge's home sequence).
// ===========================================================================

void NexusUiBridgeAdapter::HandleOpenNexusSettingsOnUI(
    ::nexus::generated::OpenNexusSettingsInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  // Section is optional; invalid / missing values fall back to "models"
  // so the front-end gets a sensible default.
  const std::string_view section =
      input.section.has_value() && IsValidSettingsSection(*input.section)
          ? std::string_view(*input.section)
          : std::string_view("models");

  if (!OpenNexusSettingsForProfile(profile_, section)) {
    std::move(done).Error("UI_ERROR", "Failed to open Nexus settings");
    return;
  }
  std::move(done).SuccessVoid();
}

void NexusUiBridgeAdapter::HandleGetProviderMissingPromptOnUI(
    ::nexus::generated::GetProviderMissingPromptInput /*input*/,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  base::DictValue header;
  header.Set("iconSvg", kProviderMissingHeaderIconSvg);
  header.Set("title", kProviderMissingHeaderTitle);
  header.Set("caption", kProviderMissingHeaderCaption);

  base::DictValue dialog;
  dialog.Set("title", kProviderMissingDialogTitle);
  dialog.Set("description", kProviderMissingDialogDescription);
  dialog.Set("confirmLabel", kProviderMissingDialogConfirmLabel);
  dialog.Set("cancelLabel", kProviderMissingDialogCancelLabel);

  base::DictValue result;
  result.Set("header", std::move(header));
  result.Set("dialog", std::move(dialog));
  std::move(done).Success(std::move(result));
}

// HandleDismissBootWelcomeOnUI + HandleShowMainContentsOnUI were
// pure C++ pass-through relays. Deleted alongside their `.api.json`
// declarations and BUILD.gn entries; the SharedWorker now fans
// `nexus.ui.dismissBootWelcome` / `nexus.ui.showMainContents`
// directly to attached WebUI bridge ports. See
// `SharedWorkerSessionManager`'s `ui.broadcastEvent` message handler
// for the worker side and `nexus_ui_event_client.ts` for the panel-
// side subscriber.

void NexusUiBridgeAdapter::HandleOpenUrlOnUI(
    ::nexus::generated::OpenUrlInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  // The guest routes in-conversation hyperlink clicks here so the click
  // does not navigate the nexus guest <webview> away (which would unload
  // the whole React SPA). Open the link in a normal (non-kNexus) browser
  // window instead. URLs originate from untrusted conversation content, so
  // only valid http/https URLs are allowed.
  const GURL url(input.url);
  if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS()) {
    std::move(done).Error("UI_ERROR",
                          "openUrl: only valid http/https URLs are allowed");
    return;
  }
  custom_browser::window_util::OpenUrlInNormalWindow(profile_, url);
  std::move(done).SuccessVoid();
}

void NexusUiBridgeAdapter::HandleOpenBrowserWindowOnUI(
    ::nexus::generated::OpenBrowserWindowInput /*input*/,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  // Backs the React topbar's "Open browser window" button. Mirrors the
  // retired Lit topbar sysbtn path (UIViewHandler::OpenBrowserWindow):
  // focus an existing normal browser window for the profile, or open a new
  // one. Deliberately does NOT change this pinned Nexus window's UI mode.
  custom_browser::window_util::OpenOrFocusNormalWindow(profile_);
  std::move(done).SuccessVoid();
}

void NexusUiBridgeAdapter::HandleOpenLanguageSettingsOnUI(
    ::nexus::generated::OpenLanguageSettingsInput /*input*/,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  // Backs the React Settings · General "Open language settings" button. Opens
  // Chrome's native languages page in a normal (non-kNexus) window so the user
  // can change the browser/UI language — Chrome owns the language list and the
  // restart-to-apply flow. This is a TRUSTED, fixed internal URL, so it goes
  // straight through OpenUrlInNormalWindow rather than HandleOpenUrlOnUI (which
  // rejects chrome:// because its URLs come from untrusted conversation links).
  custom_browser::window_util::OpenUrlInNormalWindow(
      profile_, GURL("chrome://settings/languages"));
  std::move(done).SuccessVoid();
}

void WriteNexusClipboardText(const std::string& text,
                             base::OnceCallback<void(bool)> done) {
  ui::Clipboard* clipboard = ui::Clipboard::GetForCurrentThread();
  if (!clipboard) {
    std::move(done).Run(false);
    return;
  }
  std::u16string wanted = base::UTF8ToUTF16(text);
  {
    // The write lands when the writer goes out of scope.
    ui::ScopedClipboardWriter writer(ui::ClipboardBuffer::kCopyPaste);
    writer.WriteText(wanted);
  }
  clipboard->ReadText(
      ui::ClipboardBuffer::kCopyPaste, /*data_dst=*/std::nullopt,
      base::BindOnce(
          [](std::u16string wanted, base::OnceCallback<void(bool)> done,
             std::u16string held) { std::move(done).Run(held == wanted); },
          std::move(wanted), std::move(done)));
}

void NexusUiBridgeAdapter::HandleWriteClipboardOnUI(
    ::nexus::generated::WriteClipboardInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  // A mod's `$.ui.copy` (F-25): the first of the copy's paths, the one that
  // needs neither the guest document's focus nor a user gesture. A failure
  // is an error, not a quiet success, so the guest tries the page's paths.
  WriteNexusClipboardText(
      input.text, base::BindOnce(
                      [](::nexus::BridgeResponseCallback done, bool is_copied) {
                        if (is_copied) {
                          std::move(done).SuccessVoid();
                        } else {
                          std::move(done).Error(
                              "UI_ERROR",
                              "writeClipboard: the clipboard did not take the "
                              "text");
                        }
                      },
                      std::move(done)));
}

void NexusUiBridgeAdapter::HandleSetActiveSessionTitleOnUI(
    ::nexus::generated::SetActiveSessionTitleInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  // The guest publishes the active session's resolved display title on every
  // change (boot, switch, rename, auto-title). The final native title
  // ("{title} - {product}") is composed by CustomBrowserView::GetWindowTitle;
  // this handler only sanitizes and stores the raw session title.
  //
  // Titles are derived from prompt text (semi-untrusted): collapse whitespace
  // runs (incl. newlines) and cap the length so a hostile first prompt can't
  // garble the taskbar / alt-tab text.
  std::u16string title =
      base::CollapseWhitespace(base::UTF8ToUTF16(input.title),
                               /*trim_sequences_with_line_breaks=*/true);
  if (title.size() > kMaxNexusWindowTitleLength) {
    title.resize(kMaxNexusWindowTitleLength);
    // Never leave an unpaired lead surrogate as the last unit.
    if (title.back() >= 0xD800 && title.back() <= 0xDBFF) {
      title.pop_back();
    }
  }

  // Fire-and-forget like setWindowDragRegions: no error when no kNexus window
  // exists (e.g. mid-close) — the guest republishes on next mount.
  if (Browser* nexus =
          custom_browser::window_util::FindNexusWindow(profile_)) {
    if (auto* view = static_cast<CustomBrowserView*>(
            BrowserView::GetBrowserViewForBrowser(nexus))) {
      view->SetNexusSessionTitle(title);
    }
  }
  std::move(done).SuccessVoid();
}

void NexusUiBridgeAdapter::HandleSetWindowDragRegionsOnUI(
    ::nexus::generated::SetWindowDragRegionsInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  // Payload: { regions: [{x,y,width,height,draggable}] } in guest-viewport CSS
  // px. `draggable:true` rects form the drag bar; `draggable:false` rects are
  // the interactive controls, subtracted back out so clicks reach them.
  std::vector<gfx::Rect> draggable;
  std::vector<gfx::Rect> subtract;
  if (const base::ListValue* regions = input.payload.FindList("regions")) {
    for (const base::Value& entry_value : *regions) {
      const base::DictValue* r = entry_value.GetIfDict();
      if (!r) {
        continue;
      }
      // Values arrive as JSON integers; tolerate doubles defensively.
      auto px = [&](const char* key) -> std::optional<int> {
        if (std::optional<int> i = r->FindInt(key)) {
          return i;
        }
        if (std::optional<double> d = r->FindDouble(key)) {
          return static_cast<int>(*d + (*d < 0 ? -0.5 : 0.5));
        }
        return std::nullopt;
      };
      std::optional<int> x = px("x");
      std::optional<int> y = px("y");
      std::optional<int> w = px("width");
      std::optional<int> h = px("height");
      if (!x || !y || !w || !h || *w <= 0 || *h <= 0) {
        continue;
      }
      gfx::Rect rect(*x, *y, *w, *h);
      if (r->FindBool("draggable").value_or(false)) {
        draggable.push_back(rect);
      } else {
        subtract.push_back(rect);
      }
    }
  }

  ApplyGuestDragRegionForProfile(profile_, draggable, subtract);
  std::move(done).SuccessVoid();
}

void NexusUiBridgeAdapter::HandleGetBootTimingsOnUI(
    ::nexus::generated::GetBootTimingsInput /*input*/,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  // Returns wall-clock millisecond timestamps the C++ side has captured
  // during boot, so the React side can correlate them with its own marks
  // (see globalThis.__nexusBootMarks in main.tsx / index.html). Legacy
  // pre-React timings sourced from the now-deleted chrome://glic side-
  // panel container are reported as 0 to keep the wire shape stable.
  base::DictValue result;
  result.Set("nexusViewContainerFirstCreatedMs", 0.0);
  result.Set("loadInitialUrlReturnedMs", 0.0);
  result.Set("glicNavigationCommittedMs", 0.0);
  result.Set(
      "bridgeRespondedMs",
      static_cast<double>(base::Time::Now().InMillisecondsSinceUnixEpoch()));
  std::move(done).Success(std::move(result));
}

void NexusUiBridgeAdapter::HandleGetInspectMenuEnabledOnUI(
    ::nexus::generated::GetInspectMenuEnabledInput /*input*/,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  PrefService* pref_service = profile_ ? profile_->GetPrefs() : nullptr;
  if (!pref_service) {
    std::move(done).Error("UI_ERROR", "profile prefs unavailable");
    return;
  }
  base::DictValue result;
  result.Set("enabled",
             pref_service->GetBoolean(prefs::kNexusInspectMenuEnabled));
  std::move(done).Success(base::Value(std::move(result)));
}

void NexusUiBridgeAdapter::HandleSetInspectMenuEnabledOnUI(
    ::nexus::generated::SetInspectMenuEnabledInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  PrefService* pref_service = profile_ ? profile_->GetPrefs() : nullptr;
  if (!pref_service) {
    std::move(done).Error("UI_ERROR", "profile prefs unavailable");
    return;
  }
  // Read at right-click time by MaybeHandleNexusGuestContextMenu, so the
  // flip applies to the very next right-click — no reload or restart.
  pref_service->SetBoolean(prefs::kNexusInspectMenuEnabled, input.enabled);
  base::DictValue result;
  result.Set("ok", true);
  result.Set("enabled", input.enabled);
  std::move(done).Success(base::Value(std::move(result)));
}

// HandleBroadcastCurrentSessionStateOnUI +
// HandleBroadcastSessionsSummaryOnUI were pure C++ pass-through
// relays. Deleted alongside their `.api.json` declarations; the
// SharedWorker now broadcasts the same payloads directly through
// the WebUI bridge port (one hop instead of three). See
// `nexus_session_state_client.ts` for the panel-side subscriber and
// `SharedWorkerSessionManager.flushCurrentSnapshot/flushSummary` for
// the worker firing site.

gfx::Image GetNexusNotificationHeaderIcon() {
  // The branded product logo (IDR_PRODUCT_LOGO_64 — the Nexus "V") drawn inset
  // within an 18-dip badge. This is the toast HEADER icon (the small image next
  // to the product name), replacing message_center's default `kProductIcon` (the
  // unbranded Chromium spiral). On non-ChromeOS (Windows) message_center renders
  // the small_image as-is (GenerateMaskedSmallIcon only accent-masks on
  // ChromeOS), so the full-color brand logo shows directly; NexusHeaderIconSource
  // adds the inset so it matches the default icon's footprint instead of
  // filling the box edge-to-edge.
  return gfx::Image(
      gfx::CanvasImageSource::MakeImageSkia<NexusHeaderIconSource>());
}

message_center::Notification BuildNexusToastNotification(
    const std::optional<std::string>& title,
    const std::string& message,
    const std::optional<std::string>& notification_type,
    scoped_refptr<message_center::NotificationDelegate> delegate) {
  // The toast HEADER text (display_source, "<name> • now") and the fallback
  // bold line are the user-visible product name from branding_constants.h —
  // never the internal "Nexus" codename.
  const std::u16string product_name =
      base::ASCIIToUTF16(custom_browser::kCustomBrowserProductName);
  const std::u16string bold_line = title && !title->empty()
                                       ? base::UTF8ToUTF16(*title)
                                       : product_name;
  // Stable id keyed on the category so repeated notifications of the same
  // kind (e.g. successive turn-completions) replace rather than stack.
  const std::string category =
      notification_type && !notification_type->empty() ? *notification_type
                                                       : "default";
  const std::string notification_id = "nexus." + category;

  // `icon` (the 5th arg) is the LARGE image message_center draws on the RIGHT
  // of the toast — left empty so the toast stays compact (an empty icon hides
  // the right-side image entirely; see NotificationViewBase::CreateOrUpdateIconView).
  message_center::Notification notification(
      message_center::NOTIFICATION_TYPE_SIMPLE, notification_id, bold_line,
      base::UTF8ToUTF16(message),
      /*icon=*/ui::ImageModel(), /*display_source=*/product_name, GURL(),
      message_center::NotifierId(message_center::NotifierType::SYSTEM_COMPONENT,
                                 "nexus"),
      message_center::RichNotificationData(), std::move(delegate));
  // The small header icon (next to the product name) otherwise falls back to
  // message_center's kProductIcon — the unbranded Chromium spiral. Set it to the
  // branded Nexus logo instead.
  notification.SetSmallImage(GetNexusNotificationHeaderIcon());
  return notification;
}

void NexusUiBridgeAdapter::HandleShowNotificationOnUI(
    ::nexus::generated::ShowNotificationInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  // The SharedWorker fires this on every turn-end (phase → idle). We only
  // want to surface an OS notification when the user isn't actively looking
  // at Nexus, so suppress it whenever the Nexus browser window is the active
  // window. We consult real OS window activation rather than an idle timer.
  if (IsNexusBrowserWindowActiveForProfile(profile_)) {
    std::move(done).SuccessVoid();
    return;
  }

  NotificationDisplayService* service =
      profile_ ? NotificationDisplayServiceFactory::GetForProfile(profile_)
               : nullptr;
  if (!service) {
    std::move(done).Error("UI_ERROR", "No notification display service");
    return;
  }

  // Clicking the toast brings the Nexus browser window back to the
  // foreground. The profile is bound as an opaque identity token (see
  // ActivateNexusWindowForProfileId) so this deferred click can never
  // dereference a stale Profile*. A non-null delegate is also required:
  // NotificationDisplayServiceImpl::Display DCHECKs that a TRANSIENT
  // notification carries one.
  base::RepeatingClosure on_click = base::BindRepeating(
      &ActivateNexusWindowForProfileId,
      reinterpret_cast<uintptr_t>(profile_.get()));
  message_center::Notification notification = BuildNexusToastNotification(
      input.title, input.message, input.notificationType,
      base::MakeRefCounted<message_center::HandleNotificationClickDelegate>(
          on_click));

  service->Display(NotificationHandler::Type::TRANSIENT, notification,
                   /*metadata=*/nullptr);
  std::move(done).SuccessVoid();
}

void NexusUiBridgeAdapter::HandlePickProjectFolderOnUI(
    ::nexus::generated::PickProjectFolderInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  // Test seam: e2e (Playwright) cannot drive a native OS dialog, so when
  // NEXUS_E2E_PROJECT_FOLDER is set we resolve with that path directly,
  // bypassing the dialog entirely. Production never sets this var.
  std::unique_ptr<base::Environment> env = base::Environment::Create();
  if (std::optional<std::string> seam =
          env->GetVar("NEXUS_E2E_PROJECT_FOLDER");
      seam.has_value() && !seam->empty()) {
    base::DictValue result;
    result.Set("path", *seam);
    result.Set("canceled", false);
    std::move(done).Success(std::move(result));
    return;
  }

  // Single-in-flight: only one folder dialog at a time. A non-null
  // `select_folder_dialog_` means one is already open — reject the new call
  // (its promise must still settle, so Error rather than silently drop).
  if (select_folder_dialog_) {
    std::move(done).Error("UI_ERROR",
                          "pickProjectFolder: a folder dialog is already open");
    return;
  }

  select_folder_dialog_ =
      ui::SelectFileDialog::Create(this, /*policy=*/nullptr);
  if (!select_folder_dialog_) {
    std::move(done).Error("UI_ERROR",
                          "pickProjectFolder: could not create file dialog");
    return;
  }

  // Stash the response; it fires from FileSelected / FileSelectionCanceled.
  pending_pick_done_ = std::move(done);

  const std::u16string title = input.title && !input.title->empty()
                                   ? base::UTF8ToUTF16(*input.title)
                                   : std::u16string();
  ui::SelectFileDialog::FileTypeInfo file_types;
  file_types.allowed_paths = ui::SelectFileDialog::FileTypeInfo::NATIVE_PATH;
  select_folder_dialog_->SelectFile(
      ui::SelectFileDialog::SELECT_FOLDER, title,
      /*default_path=*/base::FilePath(), &file_types, /*file_type_index=*/0,
      /*default_extension=*/base::FilePath::StringType(),
      GetNexusOwningWindowForProfile(profile_), /*caller=*/nullptr);
}

void NexusUiBridgeAdapter::HandlePickAttachmentFilesOnUI(
    ::nexus::generated::PickAttachmentFilesInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  // Test seam: e2e (Playwright) cannot drive a native OS dialog, so when
  // NEXUS_E2E_ATTACHMENT_FILES is set we resolve with those paths directly
  // (newline-separated, blank lines ignored), bypassing the dialog entirely.
  // Production never sets this var.
  std::unique_ptr<base::Environment> env = base::Environment::Create();
  if (std::optional<std::string> seam =
          env->GetVar("NEXUS_E2E_ATTACHMENT_FILES");
      seam.has_value() && !seam->empty()) {
    base::ListValue paths;
    for (const std::string& line :
         base::SplitString(*seam, "\n", base::TRIM_WHITESPACE,
                           base::SPLIT_WANT_NONEMPTY)) {
      paths.Append(line);
    }
    base::DictValue result;
    result.Set("canceled", paths.empty());
    result.Set("paths", std::move(paths));
    std::move(done).Success(std::move(result));
    return;
  }

  // Single-in-flight: only one attachment dialog at a time. A non-null
  // `select_files_dialog_` means one is already open — reject the new call
  // (its promise must still settle, so Error rather than silently drop).
  if (select_files_dialog_) {
    std::move(done).Error(
        "UI_ERROR", "pickAttachmentFiles: a file dialog is already open");
    return;
  }

  select_files_dialog_ = ui::SelectFileDialog::Create(this, /*policy=*/nullptr);
  if (!select_files_dialog_) {
    std::move(done).Error("UI_ERROR",
                          "pickAttachmentFiles: could not create file dialog");
    return;
  }

  // Stash the response; it fires from MultiFilesSelected / FileSelectionCanceled.
  pending_files_done_ = std::move(done);

  const std::u16string title = input.title && !input.title->empty()
                                   ? base::UTF8ToUTF16(*input.title)
                                   : std::u16string();

  // Optional extension filter (a UX hint — the guest composer re-validates each
  // picked file). Always keep an "All files" option so nothing is unpickable.
  ui::SelectFileDialog::FileTypeInfo file_types;
  file_types.allowed_paths = ui::SelectFileDialog::FileTypeInfo::NATIVE_PATH;
  file_types.include_all_files = true;
  if (input.extensions && !input.extensions->empty()) {
    std::vector<base::FilePath::StringType> group;
    for (const std::string& raw : *input.extensions) {
      std::string_view ext(raw);
      while (!ext.empty() && ext.front() == '.') {
        ext.remove_prefix(1);
      }
      if (!ext.empty()) {
        group.push_back(base::FilePath::FromUTF8Unsafe(ext).value());
      }
    }
    if (!group.empty()) {
      file_types.extensions.push_back(std::move(group));
    }
  }
  // file_type_index is 1-based; 1 selects our extension group when present,
  // 0 lets the dialog default (no extension group ⇒ "All files").
  const int file_type_index = file_types.extensions.empty() ? 0 : 1;

  select_files_dialog_->SelectFile(
      ui::SelectFileDialog::SELECT_OPEN_MULTI_FILE, title,
      /*default_path=*/base::FilePath(), &file_types, file_type_index,
      /*default_extension=*/base::FilePath::StringType(),
      GetNexusOwningWindowForProfile(profile_), /*caller=*/nullptr);
}

void NexusUiBridgeAdapter::HandleRevealProjectFolderOnUI(
    ::nexus::generated::RevealProjectFolderInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  if (input.path.empty()) {
    std::move(done).InvalidArgs("revealProjectFolder: missing path");
    return;
  }
  const base::FilePath path = base::FilePath::FromUTF8Unsafe(input.path);

  // Test seam: e2e can't observe a native OS file-manager window, so when
  // NEXUS_E2E_REVEAL_SINK is set we record the revealed path to that file
  // instead of opening Explorer/Finder. Production never sets this var. The
  // write is blocking, so it must run on a MayBlock thread pool (not the UI
  // thread); the e2e polls the sink file for the result.
  std::unique_ptr<base::Environment> env = base::Environment::Create();
  if (std::optional<std::string> sink = env->GetVar("NEXUS_E2E_REVEAL_SINK");
      sink.has_value() && !sink->empty()) {
    base::ThreadPool::PostTask(
        FROM_HERE, {base::MayBlock()},
        base::BindOnce(
            [](base::FilePath sink_path, std::string data) {
              base::WriteFile(sink_path, data);
            },
            base::FilePath::FromUTF8Unsafe(*sink), input.path));
    std::move(done).SuccessVoid();
    return;
  }

  platform_util::ShowItemInFolder(profile_, path);
  std::move(done).SuccessVoid();
}

void NexusUiBridgeAdapter::FileSelected(const ui::SelectedFileInfo& file,
                                        int /*index*/) {
  // SelectFileDialog guarantees exactly one terminal listener call per
  // SelectFile(), and we only ever have one dialog open, so the stashed
  // response is the live request. Clear the dialog ref first (it's done).
  select_folder_dialog_ = nullptr;
  base::DictValue result;
  result.Set("path", file.path().AsUTF8Unsafe());
  result.Set("canceled", false);
  std::move(pending_pick_done_).Success(std::move(result));
}

void NexusUiBridgeAdapter::MultiFilesSelected(
    const std::vector<ui::SelectedFileInfo>& files) {
  // SELECT_OPEN_MULTI_FILE resolves here (even for a single pick). Only the
  // attachment picker uses multi-select, so this is unambiguously its response.
  select_files_dialog_ = nullptr;
  base::ListValue paths;
  for (const ui::SelectedFileInfo& file : files) {
    paths.Append(file.path().AsUTF8Unsafe());
  }
  base::DictValue result;
  result.Set("canceled", false);
  result.Set("paths", std::move(paths));
  std::move(pending_files_done_).Success(std::move(result));
}

void NexusUiBridgeAdapter::FileSelectionCanceled() {
  // Cancel is the one shared listener callback. Disambiguate by which request
  // is in flight — only one dialog is ever open at a time. Prefer the
  // attachment (multi-file) pick, then fall back to the folder pick.
  if (select_files_dialog_) {
    select_files_dialog_ = nullptr;
    base::DictValue result;
    result.Set("canceled", true);
    result.Set("paths", base::ListValue());  // empty — nothing chosen.
    std::move(pending_files_done_).Success(std::move(result));
    return;
  }
  select_folder_dialog_ = nullptr;
  base::DictValue result;
  result.Set("path", base::Value());  // null — no folder chosen.
  result.Set("canceled", true);
  std::move(pending_pick_done_).Success(std::move(result));
}

}  // namespace nexus
