// Copyright 2026 VellocLabs
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/web_window/nexus_web_window_bridge_adapter.h"

#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "custom_browser/browser/nexus/api/web_window/web_window_options.h"
#include "custom_browser/ui/view/custom_browser_window_util.h"
#include "ui/base/page_transition_types.h"
#include "url/gurl.h"

namespace nexus {

namespace errors = ::nexus::generated::web_window_errors;

namespace {

// Size of a window opened with an origin but no size.
constexpr int kDefaultWidth = 1200;
constexpr int kDefaultHeight = 800;

base::DictValue WindowIdParams(int32_t window_id) {
  base::DictValue params;
  params.Set("window_id", window_id);
  return params;
}

base::DictValue OkResult(bool ok) {
  base::DictValue result;
  result.Set("ok", ok);
  return result;
}

}  // namespace

// One open single-web window: forwards its close, its tab's main-frame
// commits and its title changes to the owning adapter. `browser_` is null
// once the browser has closed; the adapter then drops this object.
class NexusWebWindowBridgeAdapter::Window
    : public content::WebContentsObserver {
 public:
  Window(NexusWebWindowBridgeAdapter* owner, Browser* browser)
      : owner_(owner), browser_(browser), id_(browser->session_id().id()) {
    did_close_ = browser->RegisterBrowserDidClose(base::BindRepeating(
        &Window::OnBrowserDidClose, base::Unretained(this)));
    close_cancelled_ = browser->RegisterBrowserCloseCancelled(
        base::BindRepeating(&Window::OnCloseCancelled,
                            base::Unretained(this)));
    active_tab_ = browser->RegisterActiveTabDidChange(base::BindRepeating(
        &Window::OnActiveTabChanged, base::Unretained(this)));
    ObserveActiveContents();
  }

  ~Window() override = default;

  int32_t id() const { return id_; }
  bool is_open() const { return browser_ != nullptr; }
  Browser* browser() const { return browser_; }

  content::WebContents* contents() const {
    return browser_ ? browser_->tab_strip_model()->GetActiveWebContents()
                    : nullptr;
  }

  // Close requested by the owner; onClosed then reports reason 'api'.
  void CloseByApi() {
    closing_by_api_ = true;
    browser_->window()->Close();
  }

  // Close without an event: the owning connection is going away.
  void CloseSilently() {
    if (browser_) {
      did_close_ = {};
      browser_->window()->Close();
    }
  }

  base::DictValue Describe() const {
    base::DictValue row;
    row.Set("windowId", id_);
    content::WebContents* web_contents = contents();
    row.Set("url", web_contents ? web_contents->GetLastCommittedURL().spec()
                                : std::string());
    row.Set("title", web_contents ? base::UTF16ToUTF8(web_contents->GetTitle())
                                  : std::string());
    BrowserWindow* window = browser_->window();
    row.Set("state", std::string(WebWindowStateName(window->IsMaximized(),
                                                    window->IsMinimized())));
    return row;
  }

 private:
  void OnBrowserDidClose(BrowserWindowInterface* /*browser*/) {
    browser_ = nullptr;
    Observe(nullptr);
    // Last: the owner may schedule this object's deletion.
    owner_->OnWindowClosed(id_, closing_by_api_ ? "api" : "user");
  }

  void OnCloseCancelled(BrowserWindowInterface* /*browser*/,
                        BrowserWindowInterface::ClosingStatus /*status*/) {
    closing_by_api_ = false;
  }

  void OnActiveTabChanged(BrowserWindowInterface* /*browser*/) {
    ObserveActiveContents();
  }

  void ObserveActiveContents() { Observe(contents()); }

  // content::WebContentsObserver:
  void DidFinishNavigation(content::NavigationHandle* handle) override {
    if (!handle->IsInPrimaryMainFrame() || !handle->HasCommitted()) {
      return;
    }
    base::DictValue payload;
    payload.Set("url", handle->GetURL().spec());
    payload.Set("isSameDocument", handle->IsSameDocument());
    payload.Set("isErrorPage", handle->IsErrorPage());
    owner_->OnWindowNavigated(id_, std::move(payload));
  }

  void TitleWasSet(content::NavigationEntry* /*entry*/) override {
    base::DictValue payload;
    payload.Set("title", base::UTF16ToUTF8(web_contents()->GetTitle()));
    owner_->OnWindowTitleChanged(id_, std::move(payload));
  }

  raw_ptr<NexusWebWindowBridgeAdapter> owner_;
  raw_ptr<Browser> browser_;
  const int32_t id_;
  bool closing_by_api_ = false;
  base::CallbackListSubscription did_close_;
  base::CallbackListSubscription close_cancelled_;
  base::CallbackListSubscription active_tab_;
};

NexusWebWindowBridgeAdapter::NexusWebWindowBridgeAdapter(
    NexusBridgeResponseDelegate* delegate,
    Profile* profile)
    : ::nexus::generated::WebWindowApiBase(delegate), profile_(profile) {}

NexusWebWindowBridgeAdapter::~NexusWebWindowBridgeAdapter() {
  // The owning connection is gone: its windows go with it.
  for (auto& [id, window] : windows_) {
    window->CloseSilently();
  }
}

void NexusWebWindowBridgeAdapter::HandleOpenOnUI(
    ::nexus::generated::WebWindowOpenInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  const GURL url(input.url);
  if (!IsAllowedWebWindowUrl(url)) {
    base::DictValue params;
    params.Set("url", input.url);
    std::move(done).Error(errors::kUrlNotAllowed,
                          "only http, https and file URLs can be opened",
                          std::move(params));
    return;
  }

  ui::mojom::WindowShowState show_state = ui::mojom::WindowShowState::kNormal;
  if (input.state.has_value()) {
    std::optional<ui::mojom::WindowShowState> parsed =
        ParseWebWindowState(*input.state);
    if (!parsed) {
      base::DictValue params;
      params.Set("reason", "unknown state");
      std::move(done).Error(errors::kInvalidArgs, "unknown state",
                            std::move(params));
      return;
    }
    show_state = *parsed;
  }

  const bool has_origin = input.left.has_value() || input.top.has_value();
  const bool has_size = input.width.has_value() || input.height.has_value();
  gfx::Rect initial_bounds;
  if (has_origin || has_size) {
    WebWindowBoundsRequest request{input.left, input.top, input.width,
                                   input.height};
    if (std::optional<std::string> reason = ResolveWebWindowBounds(
            request, gfx::Rect(0, 0, kDefaultWidth, kDefaultHeight),
            initial_bounds)) {
      base::DictValue params;
      params.Set("reason", *reason);
      std::move(done).Error(errors::kInvalidArgs, *reason, std::move(params));
      return;
    }
  }

  if (!profile_ || Browser::GetCreationStatusForProfile(profile_) !=
                       Browser::CreationStatus::kOk) {
    std::move(done).Error(errors::kWindowCreateFailed,
                          "a window cannot be created now");
    return;
  }

  Browser::CreateParams params(Browser::TYPE_NORMAL, profile_,
                               /*user_gesture=*/true);
  params.omit_from_session_restore = true;
  params.should_trigger_session_restore = false;
  params.initial_show_state = show_state;
  if (has_origin || has_size) {
    params.initial_bounds = initial_bounds;
    params.initial_origin_specified =
        has_origin ? Browser::ValueSpecified::kSpecified
                   : Browser::ValueSpecified::kUnspecified;
  }
  // The marker is consumed by the CustomBrowserView inside this synchronous
  // Create; the second consume clears it should no view have taken it.
  custom_browser::window_util::SetPendingSingleWebWindow(profile_);
  Browser* browser = Browser::Create(params);
  custom_browser::window_util::ConsumePendingSingleWebWindow(profile_);
  if (!browser || !custom_browser::window_util::IsSingleWebBrowser(browser)) {
    if (browser) {
      browser->window()->Close();
    }
    std::move(done).Error(errors::kWindowCreateFailed,
                          "the window could not be created");
    return;
  }

  NavigateParams navigate(browser, url, ui::PAGE_TRANSITION_AUTO_TOPLEVEL);
  navigate.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
  const bool focus = input.focus.value_or(true) &&
                     show_state != ui::mojom::WindowShowState::kMinimized;
  navigate.window_action = focus
                               ? NavigateParams::WindowAction::kShowWindow
                               : NavigateParams::WindowAction::kShowWindowInactive;
  Navigate(&navigate);

  auto window = std::make_unique<Window>(this, browser);
  const int32_t window_id = window->id();
  windows_[window_id] = std::move(window);

  base::DictValue result;
  result.Set("windowId", window_id);
  std::move(done).Success(std::move(result));
}

void NexusWebWindowBridgeAdapter::HandleCloseOnUI(
    ::nexus::generated::WebWindowCloseInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  Window* window = FindOrReject(input.windowId, done);
  if (!window) {
    return;
  }
  window->CloseByApi();
  std::move(done).Success(OkResult(true));
}

void NexusWebWindowBridgeAdapter::HandleListOnUI(
    ::nexus::generated::WebWindowListInput /*input*/,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  base::ListValue rows;
  for (const auto& [id, window] : windows_) {
    if (window->is_open()) {
      rows.Append(window->Describe());
    }
  }
  base::DictValue result;
  result.Set("windows", std::move(rows));
  std::move(done).Success(std::move(result));
}

void NexusWebWindowBridgeAdapter::HandleFocusOnUI(
    ::nexus::generated::WebWindowFocusInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  Window* window = FindOrReject(input.windowId, done);
  if (!window) {
    return;
  }
  BrowserWindow* browser_window = window->browser()->window();
  if (browser_window->IsMinimized()) {
    browser_window->Restore();
  }
  browser_window->Show();
  browser_window->Activate();
  std::move(done).Success(OkResult(true));
}

void NexusWebWindowBridgeAdapter::HandleSetBoundsOnUI(
    ::nexus::generated::WebWindowSetBoundsInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  Window* window = FindOrReject(input.windowId, done);
  if (!window) {
    return;
  }
  BrowserWindow* browser_window = window->browser()->window();
  WebWindowBoundsRequest request{input.left, input.top, input.width,
                                 input.height};
  gfx::Rect bounds;
  if (std::optional<std::string> reason = ResolveWebWindowBounds(
          request, browser_window->GetRestoredBounds(), bounds)) {
    base::DictValue params;
    params.Set("reason", *reason);
    std::move(done).Error(errors::kInvalidArgs, *reason, std::move(params));
    return;
  }
  if (browser_window->IsMaximized() || browser_window->IsMinimized()) {
    browser_window->Restore();
  }
  browser_window->SetBounds(bounds);
  std::move(done).Success(OkResult(true));
}

void NexusWebWindowBridgeAdapter::HandleSetStateOnUI(
    ::nexus::generated::WebWindowSetStateInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  std::optional<ui::mojom::WindowShowState> state =
      ParseWebWindowState(input.state);
  if (!state) {
    base::DictValue params;
    params.Set("reason", "unknown state");
    std::move(done).Error(errors::kInvalidArgs, "unknown state",
                          std::move(params));
    return;
  }
  Window* window = FindOrReject(input.windowId, done);
  if (!window) {
    return;
  }
  BrowserWindow* browser_window = window->browser()->window();
  switch (*state) {
    case ui::mojom::WindowShowState::kMaximized:
      browser_window->Maximize();
      break;
    case ui::mojom::WindowShowState::kMinimized:
      browser_window->Minimize();
      break;
    default:
      browser_window->Restore();
      break;
  }
  std::move(done).Success(OkResult(true));
}

void NexusWebWindowBridgeAdapter::HandleNavigateOnUI(
    ::nexus::generated::WebWindowNavigateInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  const GURL url(input.url);
  if (!IsAllowedWebWindowUrl(url)) {
    base::DictValue params;
    params.Set("url", input.url);
    std::move(done).Error(errors::kUrlNotAllowed,
                          "only http, https and file URLs can be opened",
                          std::move(params));
    return;
  }
  Window* window = FindOrReject(input.windowId, done);
  if (!window) {
    return;
  }
  content::WebContents* web_contents = window->contents();
  if (!web_contents) {
    std::move(done).Error(errors::kWindowNotFound, "the window has no tab",
                          WindowIdParams(input.windowId));
    return;
  }
  content::NavigationController::LoadURLParams load(url);
  load.transition_type = ui::PAGE_TRANSITION_AUTO_TOPLEVEL;
  web_contents->GetController().LoadURLWithParams(load);
  std::move(done).Success(OkResult(true));
}

void NexusWebWindowBridgeAdapter::HandleBackOnUI(
    ::nexus::generated::WebWindowBackInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  Window* window = FindOrReject(input.windowId, done);
  if (!window) {
    return;
  }
  content::WebContents* web_contents = window->contents();
  const bool can = web_contents && web_contents->GetController().CanGoBack();
  if (can) {
    web_contents->GetController().GoBack();
  }
  std::move(done).Success(OkResult(can));
}

void NexusWebWindowBridgeAdapter::HandleForwardOnUI(
    ::nexus::generated::WebWindowForwardInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  Window* window = FindOrReject(input.windowId, done);
  if (!window) {
    return;
  }
  content::WebContents* web_contents = window->contents();
  const bool can =
      web_contents && web_contents->GetController().CanGoForward();
  if (can) {
    web_contents->GetController().GoForward();
  }
  std::move(done).Success(OkResult(can));
}

void NexusWebWindowBridgeAdapter::HandleReloadOnUI(
    ::nexus::generated::WebWindowReloadInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  Window* window = FindOrReject(input.windowId, done);
  if (!window) {
    return;
  }
  if (content::WebContents* web_contents = window->contents()) {
    web_contents->GetController().Reload(content::ReloadType::NORMAL,
                                         /*check_for_repost=*/true);
  }
  std::move(done).Success(OkResult(true));
}

NexusWebWindowBridgeAdapter::Window* NexusWebWindowBridgeAdapter::FindOrReject(
    int32_t window_id,
    ::nexus::BridgeResponseCallback& done) {
  auto it = windows_.find(window_id);
  if (it == windows_.end() || !it->second->is_open()) {
    std::move(done).Error(errors::kWindowNotFound,
                          "no such window on this connection",
                          WindowIdParams(window_id));
    return nullptr;
  }
  return it->second.get();
}

void NexusWebWindowBridgeAdapter::OnWindowNavigated(int32_t window_id,
                                                    base::DictValue payload) {
  payload.Set("windowId", window_id);
  PushEvent("onNavigated", std::move(payload));
}

void NexusWebWindowBridgeAdapter::OnWindowTitleChanged(
    int32_t window_id,
    base::DictValue payload) {
  payload.Set("windowId", window_id);
  PushEvent("onTitleChanged", std::move(payload));
}

void NexusWebWindowBridgeAdapter::OnWindowClosed(int32_t window_id,
                                                 std::string_view reason) {
  base::DictValue payload;
  payload.Set("windowId", window_id);
  payload.Set("reason", reason);
  PushEvent("onClosed", std::move(payload));
  // The Window is inside its own close callback; drop it afterwards.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::WeakPtr<NexusWebWindowBridgeAdapter> self,
                        int32_t id) {
                       if (self) {
                         self->windows_.erase(id);
                       }
                     },
                     weak_factory_.GetWeakPtr(), window_id));
}

void NexusWebWindowBridgeAdapter::PushEvent(std::string_view action,
                                            base::DictValue payload) {
  base::DictValue route;
  route.Set("namespace", "nexus");
  route.Set("capability", "webWindow");
  route.Set("action", action);
  SendEvent(route, base::Value(std::move(payload)));
}

}  // namespace nexus
