#include "custom_browser/browser/nexus/ui/nexus_ui_bridge_adapter.h"

#include <string_view>
#include <utility>

#include "base/time/time.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "custom_browser/browser/nexus/ui/nexus_ui_broadcast.h"
#include "custom_browser/ui/view/custom_browser_view.h"
#include "custom_browser/ui/web_ui/custom_browser_webui_view.h"

namespace nexus {

namespace {

bool IsValidSettingsSection(std::string_view section) {
  return section == "models" || section == "webapp";
}

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
    : NexusBridgeCapabilityHandler(delegate), profile_(profile) {
  Register("openNexusSettings", &NexusUiBridgeAdapter::HandleOpenNexusSettings);
  Register("getProviderMissingPrompt",
           &NexusUiBridgeAdapter::HandleGetProviderMissingPrompt);
  Register("dismissBootWelcome",
           &NexusUiBridgeAdapter::HandleDismissBootWelcome);
  Register("showMainContents",
           &NexusUiBridgeAdapter::HandleShowMainContents);
  Register("getBootTimings",
           &NexusUiBridgeAdapter::HandleGetBootTimings);
}

NexusUiBridgeAdapter::~NexusUiBridgeAdapter() = default;

void NexusUiBridgeAdapter::HandleOpenNexusSettings(
    const std::string& id,
    const base::DictValue& route,
    const base::DictValue& payload,
    std::vector<blink::MessagePortDescriptor> /*ports*/) {
  const std::string* requested_section = payload.FindString("section");
  const std::string_view section =
      requested_section && IsValidSettingsSection(*requested_section)
          ? std::string_view(*requested_section)
          : std::string_view("models");

  if (!OpenNexusSettingsForProfile(profile_, section)) {
    SendErrorResponse(id, route, "UI_ERROR",
                      "Failed to open Nexus settings");
    return;
  }

  base::DictValue result;
  result.Set("ok", true);
  SendSuccessResponse(id, route, base::Value(std::move(result)));
}

void NexusUiBridgeAdapter::HandleGetProviderMissingPrompt(
    const std::string& id,
    const base::DictValue& route,
    const base::DictValue& /*payload*/,
    std::vector<blink::MessagePortDescriptor> /*ports*/) {
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
  SendSuccessResponse(id, route, base::Value(std::move(result)));
}

void NexusUiBridgeAdapter::HandleDismissBootWelcome(
    const std::string& id,
    const base::DictValue& route,
    const base::DictValue& /*payload*/,
    std::vector<blink::MessagePortDescriptor> /*ports*/) {
  // Tells the parent WebUI to dismiss its boot welcome overlay and reveal
  // <nexus-view-container>. Does NOT show <main-contents-view> or
  // <cr-splitter> — those stay hidden until the model explicitly invokes
  // `showMainContents`. Called by the React guest on initial boot once
  // sessions and worker snapshot are ready.
  BroadcastWebUiListenerEventToProfile(profile_, "nexusDismissBootWelcome");

  base::DictValue result;
  result.Set("ok", true);
  SendSuccessResponse(id, route, base::Value(std::move(result)));
}

void NexusUiBridgeAdapter::HandleShowMainContents(
    const std::string& id,
    const base::DictValue& route,
    const base::DictValue& /*payload*/,
    std::vector<blink::MessagePortDescriptor> /*ports*/) {
  // Reveals <main-contents-view> + <cr-splitter> in the parent WebUI. The
  // boot path does NOT call this — only the model code calls it (e.g. when
  // the agent decides to surface the main browser content alongside the
  // chat pane). Idempotent: the parent latches its visibility flag.
  BroadcastWebUiListenerEventToProfile(profile_, "nexusShowMainContents");

  base::DictValue result;
  result.Set("ok", true);
  SendSuccessResponse(id, route, base::Value(std::move(result)));
}

void NexusUiBridgeAdapter::HandleGetBootTimings(
    const std::string& id,
    const base::DictValue& route,
    const base::DictValue& /*payload*/,
    std::vector<blink::MessagePortDescriptor> /*ports*/) {
  // Returns wall-clock millisecond timestamps the C++ side has captured
  // during boot, so the React side can correlate them with its own marks
  // (see globalThis.__nexusBootMarks in main.tsx / index.html). Used to
  // attribute the ~1.8 s pre-React phase: how much was browser startup,
  // chrome://glic load, webview spawn, vs. React bundle fetch+parse.
  //
  //   nexusViewContainerFirstCreatedMs
  //     ms-since-epoch when the very first NexusViewContainer
  //     (a views::WebView) was constructed in this process. This is
  //     roughly when the parent WebUI first triggered chrome://glic to
  //     load. Compare to the React side's `htmlParsedMs` to see how
  //     long the chrome://glic + webview-spawn + bundle-fetch chain
  //     took.
  //
  //   bridgeRespondedMs
  //     ms-since-epoch when this handler ran. Compare to the request's
  //     dispatch time on the React side to measure bridge round-trip.
  // Legacy pre-React timings sourced from the now-deleted chrome://glic
  // side-panel container are reported as 0 to keep the wire shape stable
  // for the React-side `globalThis.__nexusBootMarks` consumers in
  // main.tsx / index.html.
  base::DictValue result;
  result.Set("nexusViewContainerFirstCreatedMs", 0.0);
  result.Set("loadInitialUrlReturnedMs", 0.0);
  result.Set("glicNavigationCommittedMs", 0.0);
  result.Set(
      "bridgeRespondedMs",
      static_cast<double>(base::Time::Now().InMillisecondsSinceUnixEpoch()));
  SendSuccessResponse(id, route, base::Value(std::move(result)));
}

}  // namespace nexus
