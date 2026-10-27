// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/web_window/nexus_web_window_bridge_adapter.h"

#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/timer/timer.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "custom_browser/browser/nexus/api/web_window/sign_in_status_page.h"
#include "custom_browser/browser/nexus/api/web_window/sign_in_window.h"
#include "custom_browser/browser/nexus/api/web_window/single_web_window.h"
#include "custom_browser/browser/nexus/api/web_window/web_window_options.h"
#include "custom_browser/browser/nexus/api/web_window/web_window_redirect_capture.h"
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
  Window(NexusWebWindowBridgeAdapter* owner, BrowserWindowInterface* browser)
      : owner_(owner), browser_(browser), id_(browser->GetSessionID().id()) {
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
  BrowserWindowInterface* browser() const { return browser_; }

  content::WebContents* contents() const {
    return browser_ ? browser_->tab_strip_model()->GetActiveWebContents()
                    : nullptr;
  }

  // Close requested by the owner; onClosed then reports reason 'api'.
  void CloseByApi() {
    closing_by_api_ = true;
    BrowserWindow::FromBrowser(browser_)->Close();
  }

  // awaitSignInResult: the captured sign-in waits for showSignInResult.
  void set_awaits_sign_in() { awaits_sign_in_ = true; }
  bool awaits_sign_in() const { return awaits_sign_in_; }
  bool sign_in_pending() const { return sign_in_pending_; }

  // The capture fired: the host's "Finishing sign-in" page until the result.
  void ShowSignInFinishing() {
    sign_in_pending_ = true;
    ShowSignInStatus(contents(), SignInStatus::kFinishing);
  }

  // Success closes the window after kSignInSuccessHold; failure stays open.
  void ShowSignInResult(bool ok, std::u16string_view message) {
    sign_in_pending_ = false;
    ShowSignInStatus(contents(),
                     ok ? SignInStatus::kSucceeded : SignInStatus::kFailed,
                     ok ? std::u16string_view() : message);
    if (ok) {
      close_timer_.Start(FROM_HERE, kSignInSuccessHold,
                         base::BindOnce(&Window::CloseByApi,
                                        base::Unretained(this)));
    }
  }

  // Close without an event: the owning connection is going away.
  void CloseSilently() {
    if (browser_) {
      did_close_ = {};
      BrowserWindow::FromBrowser(browser_)->Close();
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
    BrowserWindow* window = BrowserWindow::FromBrowser(browser_);
    row.Set("state", std::string(WebWindowStateName(window->IsMaximized(),
                                                    window->IsMinimized())));
    return row;
  }

 private:
  void OnBrowserDidClose(BrowserWindowInterface* /*browser*/) {
    browser_ = nullptr;
    close_timer_.Stop();
    Observe(nullptr);
    // Last: the owner may schedule this object's deletion.
    owner_->OnWindowClosed(id_, closing_by_api_ ? "api" : "user");
  }

  // kDeniedUnloadHandlersNeedTime is the first pass of a close that runs the
  // page's unload handlers; the close is still under way.
  void OnCloseCancelled(BrowserWindowInterface* /*browser*/,
                        BrowserWindowInterface::ClosingStatus status) {
    if (status !=
        BrowserWindowInterface::ClosingStatus::kDeniedUnloadHandlersNeedTime) {
      closing_by_api_ = false;
    }
  }

  void OnActiveTabChanged(BrowserWindowInterface* /*browser*/) {
    ObserveActiveContents();
  }

  void ObserveActiveContents() { Observe(contents()); }

  // content::WebContentsObserver:
  // The person chose to stay on the page: the close was cancelled.
  void BeforeUnloadFired(bool proceed) override {
    if (!proceed) {
      closing_by_api_ = false;
    }
  }

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
  raw_ptr<BrowserWindowInterface> browser_;
  const int32_t id_;
  bool closing_by_api_ = false;
  bool awaits_sign_in_ = false;
  bool sign_in_pending_ = false;
  base::OneShotTimer close_timer_;
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

  std::optional<GURL> capture_target;
  if (input.captureRedirect.has_value()) {
    GURL target(*input.captureRedirect);
    std::string_view reason;
    if (!IsValidWebWindowCaptureTarget(target)) {
      reason = "captureRedirect must be a loopback http URL";
    } else if (MatchesWebWindowCaptureTarget(target, url)) {
      reason = "url must not match captureRedirect";
    }
    if (!reason.empty()) {
      base::DictValue params;
      params.Set("reason", reason);
      std::move(done).Error(errors::kInvalidArgs, std::string(reason),
                            std::move(params));
      return;
    }
    capture_target = std::move(target);
  }
  const bool await_sign_in = input.awaitSignInResult.value_or(false);
  if (await_sign_in && !capture_target) {
    base::DictValue params;
    params.Set("reason", "awaitSignInResult requires captureRedirect");
    std::move(done).Error(errors::kInvalidArgs,
                          "awaitSignInResult requires captureRedirect",
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

  std::u16string fixed_title;
  if (input.title.has_value()) {
    std::optional<std::u16string> normalized =
        NormalizeWebWindowTitle(*input.title);
    if (!normalized) {
      base::DictValue params;
      params.Set("reason", "title must be 1 to 120 characters");
      std::move(done).Error(errors::kInvalidArgs,
                            "title must be 1 to 120 characters",
                            std::move(params));
      return;
    }
    fixed_title = std::move(*normalized);
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

  SingleWebWindowParams params;
  params.title = std::move(fixed_title);
  if (has_origin || has_size) {
    params.bounds = initial_bounds;
    params.origin_specified = has_origin;
  }
  params.show_state = show_state;
  params.focus = input.focus.value_or(true);
  BrowserWindowInterface* browser = OpenSingleWebWindow(profile_, url, params);
  if (!browser) {
    std::move(done).Error(errors::kWindowCreateFailed,
                          "the window could not be created");
    return;
  }

  auto window = std::make_unique<Window>(this, browser);
  const int32_t window_id = window->id();
  if (capture_target) {
    // Security: an unarmed window would let the redirect reach the port.
    content::WebContents* web_contents = window->contents();
    if (!web_contents) {
      window->CloseSilently();
      std::move(done).Error(errors::kWindowCreateFailed,
                            "the window has no tab");
      return;
    }
    if (await_sign_in) {
      window->set_awaits_sign_in();
    }
    WebWindowRedirectCapture::Attach(
        web_contents, std::move(*capture_target),
        base::BindOnce(&NexusWebWindowBridgeAdapter::OnWindowRedirectCaptured,
                       weak_factory_.GetWeakPtr(), window_id));
  }
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

void NexusWebWindowBridgeAdapter::HandleShowSignInResultOnUI(
    ::nexus::generated::WebWindowShowSignInResultInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  Window* window = FindOrReject(input.windowId, done);
  if (!window) {
    return;
  }
  if (!window->sign_in_pending()) {
    base::DictValue params;
    params.Set("reason", "no captured sign-in is waiting for a result");
    std::move(done).Error(errors::kInvalidArgs,
                          "no captured sign-in is waiting for a result",
                          std::move(params));
    return;
  }
  window->ShowSignInResult(
      input.ok, base::UTF8ToUTF16(input.message.value_or(std::string())));
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
  BrowserWindow* browser_window = BrowserWindow::FromBrowser(window->browser());
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
  BrowserWindow* browser_window = BrowserWindow::FromBrowser(window->browser());
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
  BrowserWindow* browser_window = BrowserWindow::FromBrowser(window->browser());
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

content::WebContents* NexusWebWindowBridgeAdapter::ContentsForWindow(
    int32_t window_id) const {
  auto it = windows_.find(window_id);
  if (it == windows_.end() || !it->second->is_open()) {
    return nullptr;
  }
  return it->second->contents();
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

void NexusWebWindowBridgeAdapter::OnWindowRedirectCaptured(int32_t window_id,
                                                           const GURL& url) {
  auto it = windows_.find(window_id);
  if (it == windows_.end() || !it->second->is_open()) {
    return;
  }
  base::DictValue payload;
  payload.Set("windowId", window_id);
  payload.Set("url", url.spec());
  PushEvent("onRedirectCaptured", std::move(payload));
  if (it->second->awaits_sign_in()) {
    it->second->ShowSignInFinishing();
  } else {
    it->second->CloseByApi();
  }
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
