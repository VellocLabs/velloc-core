// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/providers/provider_sign_in_windows.h"

#include <string_view>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/notreached.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/timer/timer.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "content/public/browser/web_contents.h"
#include "custom_browser/browser/nexus/api/llm/llm_broker.h"
#include "custom_browser/browser/nexus/api/llm/llm_broker_factory.h"
#include "custom_browser/browser/nexus/api/llm/oauth/oauth_manager.h"
#include "custom_browser/browser/nexus/api/llm/oauth/oauth_providers.h"
#include "custom_browser/browser/nexus/api/web_window/sign_in_status_page.h"
#include "custom_browser/browser/nexus/api/web_window/sign_in_window.h"
#include "custom_browser/browser/nexus/api/web_window/single_web_window.h"
#include "custom_browser/browser/nexus/api/web_window/web_window_redirect_capture.h"
#include "custom_browser/resources/grit/custom_browser_strings.h"
#include "net/base/url_util.h"
#include "ui/base/l10n/l10n_util.h"
#include "url/gurl.h"

namespace nexus {

namespace {

const char kUserDataKey[] = "nexus_provider_sign_in_windows";

// The same size as the runtime's MCP sign-in window.
constexpr int kWindowWidth = 520;
constexpr int kWindowHeight = 720;

NexusOAuthManager* OAuthManagerFor(Profile* profile) {
  LlmBroker* broker = LlmBrokerFactory::GetForProfile(profile);
  return broker ? broker->oauth_manager() : nullptr;
}

// The failure page's reason line for an OAuth `error` code: a localized
// line per kind, never the code.
std::u16string OAuthFailureReason(std::string_view error) {
  switch (ClassifyOAuthSignInError(error)) {
    case SignInFailureKind::kDenied:
      return l10n_util::GetStringUTF16(IDS_SIGN_IN_STATUS_DENIED);
    case SignInFailureKind::kExpired:
      return l10n_util::GetStringUTF16(IDS_SIGN_IN_STATUS_EXPIRED);
    case SignInFailureKind::kServiceError:
      return l10n_util::GetStringUTF16(IDS_SIGN_IN_STATUS_SERVICE_ERROR);
  }
  NOTREACHED();
}

// A device-code login that ends without an error code ran out of time.
std::u16string DeviceCodeFailureReason(std::string_view error) {
  return OAuthFailureReason(error.empty() ? "expired_token" : error);
}

}  // namespace

// One open sign-in window. `on_closed` runs once, inside the browser's
// did-close notification.
class ProviderSignInWindows::Window {
 public:
  Window(BrowserWindowInterface* browser,
         uint64_t serial,
         bool device_code,
         base::OnceClosure on_closed)
      : browser_(browser),
        serial_(serial),
        device_code_(device_code),
        on_closed_(std::move(on_closed)) {
    if (content::WebContents* first =
            browser->tab_strip_model()->GetActiveWebContents()) {
      sign_in_contents_ = first->GetWeakPtr();
    }
    did_close_ = browser->RegisterBrowserDidClose(base::BindRepeating(
        &Window::OnBrowserDidClose, base::Unretained(this)));
  }

  // Closes a still-open window without running `on_closed`.
  ~Window() {
    if (browser_) {
      did_close_ = {};
      BrowserWindow::FromBrowser(browser_)->Close();
    }
  }

  Window(const Window&) = delete;
  Window& operator=(const Window&) = delete;

  uint64_t serial() const { return serial_; }
  bool device_code() const { return device_code_; }
  // Whether the login this window shows has not reported its end yet.
  bool running() const { return running_; }

  // The tab the sign-in opened in, wherever the page went later; null once
  // it is gone.
  content::WebContents* contents() const {
    return browser_ ? sign_in_contents_.get() : nullptr;
  }

  void ShowFinishing() { ShowStatus(SignInStatus::kFinishing); }

  void ShowResult(bool ok, std::u16string_view reason) {
    running_ = false;
    ShowStatus(ok ? SignInStatus::kSucceeded : SignInStatus::kFailed,
               ok ? std::u16string_view() : reason);
    if (ok) {
      close_timer_.Start(FROM_HERE, kSignInSuccessHold,
                         base::BindOnce(&Window::Close, base::Unretained(this)));
    }
  }

 private:
  // Status pages go to the sign-in's own tab, brought to the front should the
  // service have opened another tab in this window.
  void ShowStatus(SignInStatus status, std::u16string_view reason = {}) {
    content::WebContents* web_contents = contents();
    if (!web_contents) {
      return;
    }
    TabStripModel* strip = browser_->tab_strip_model();
    const int index = strip->GetIndexOfWebContents(web_contents);
    if (index != TabStripModel::kNoTab && index != strip->active_index()) {
      strip->ActivateTabAt(index);
    }
    ShowSignInStatus(web_contents, status, reason);
  }

  void Close() {
    if (browser_) {
      BrowserWindow::FromBrowser(browser_)->Close();
    }
  }

  void OnBrowserDidClose(BrowserWindowInterface* /*browser*/) {
    browser_ = nullptr;
    close_timer_.Stop();
    // Last: the owner schedules this object's deletion.
    std::move(on_closed_).Run();
  }

  raw_ptr<BrowserWindowInterface> browser_;
  base::WeakPtr<content::WebContents> sign_in_contents_;
  const uint64_t serial_;
  const bool device_code_;
  bool running_ = true;
  base::OnceClosure on_closed_;
  base::OneShotTimer close_timer_;
  base::CallbackListSubscription did_close_;
};

// static
ProviderSignInWindows* ProviderSignInWindows::GetForProfile(Profile* profile) {
  if (!profile) {
    return nullptr;
  }
  auto* windows =
      static_cast<ProviderSignInWindows*>(profile->GetUserData(kUserDataKey));
  if (!windows) {
    auto owned = std::make_unique<ProviderSignInWindows>(profile);
    windows = owned.get();
    profile->SetUserData(kUserDataKey, std::move(owned));
  }
  return windows;
}

ProviderSignInWindows::ProviderSignInWindows(Profile* profile)
    : profile_(profile) {}

ProviderSignInWindows::~ProviderSignInWindows() = default;

bool ProviderSignInWindows::OpenForRedirect(const std::string& provider_id,
                                            const GURL& auth_url) {
  Window* window = Open(provider_id, auth_url, /*device_code=*/false);
  if (!window) {
    return false;
  }
  content::WebContents* contents = window->contents();
  if (!contents) {
    windows_.erase(provider_id);
    return false;
  }
  // Security: an unarmed window would let the redirect reach the port. The
  // capture also arms every page the sign-in opens (popup or new tab).
  WebWindowRedirectCapture::Attach(
      contents, GURL(kNexusOAuthRedirectUri),
      base::BindOnce(&ProviderSignInWindows::OnRedirectCaptured,
                     weak_factory_.GetWeakPtr(), provider_id,
                     window->serial()));
  return true;
}

bool ProviderSignInWindows::OpenForDeviceCode(const std::string& provider_id,
                                              const GURL& verification_url) {
  return Open(provider_id, verification_url, /*device_code=*/true) != nullptr;
}

void ProviderSignInWindows::Discard(const std::string& provider_id) {
  windows_.erase(provider_id);
}

void ProviderSignInWindows::OnDeviceCodeDone(const std::string& provider_id,
                                             bool ok,
                                             std::string error) {
  auto it = windows_.find(provider_id);
  if (it == windows_.end() || !it->second->device_code() ||
      !it->second->running()) {
    return;
  }
  it->second->ShowResult(ok, ok ? std::u16string()
                                : DeviceCodeFailureReason(error));
}

ProviderSignInWindows::Window* ProviderSignInWindows::Open(
    const std::string& provider_id,
    const GURL& url,
    bool device_code) {
  const OAuthProviderConfig* config = GetOAuthProviderConfig(provider_id);
  if (!config || !url.is_valid() || !url.SchemeIsHTTPOrHTTPS()) {
    return nullptr;
  }
  windows_.erase(provider_id);

  SingleWebWindowParams params;
  params.title = SignInWindowTitle(base::UTF8ToUTF16(config->display_name));
  params.bounds = gfx::Rect(0, 0, kWindowWidth, kWindowHeight);
  params.origin_specified = false;
  BrowserWindowInterface* browser = OpenSingleWebWindow(profile_, url, params);
  if (!browser) {
    return nullptr;
  }
  const uint64_t serial = next_serial_++;
  auto window = std::make_unique<Window>(
      browser, serial, device_code,
      base::BindOnce(&ProviderSignInWindows::OnWindowClosed,
                     weak_factory_.GetWeakPtr(), provider_id, serial));
  Window* raw = window.get();
  windows_[provider_id] = std::move(window);
  return raw;
}

ProviderSignInWindows::Window* ProviderSignInWindows::Find(
    const std::string& provider_id,
    uint64_t serial) {
  auto it = windows_.find(provider_id);
  return it != windows_.end() && it->second->serial() == serial
             ? it->second.get()
             : nullptr;
}

void ProviderSignInWindows::OnRedirectCaptured(const std::string& provider_id,
                                               uint64_t serial,
                                               const GURL& url) {
  Window* window = Find(provider_id, serial);
  if (!window) {
    return;
  }
  std::string code;
  std::string state;
  std::string error;
  std::string error_description;
  net::GetValueForKeyInQuery(url, "code", &code);
  net::GetValueForKeyInQuery(url, "state", &state);
  net::GetValueForKeyInQuery(url, "error", &error);
  net::GetValueForKeyInQuery(url, "error_description", &error_description);

  NexusOAuthManager* manager = OAuthManagerFor(profile_);
  if (!error.empty() || code.empty() || state.empty() || !manager) {
    // The page shows a localized reason only; the service's code and text
    // go to the log.
    if (!error.empty()) {
      LOG(WARNING) << "provider sign-in for " << provider_id
                   << " failed: " << error << " " << error_description;
    }
    window->ShowResult(false, error.empty() ? std::u16string()
                                            : OAuthFailureReason(error));
    return;
  }
  window->ShowFinishing();
  manager->HandleAuthCode(
      code, state,
      base::BindOnce(&ProviderSignInWindows::OnAuthCodeExchanged,
                     weak_factory_.GetWeakPtr(), provider_id, serial));
}

void ProviderSignInWindows::OnAuthCodeExchanged(const std::string& provider_id,
                                                uint64_t serial,
                                                bool ok,
                                                std::string /*error*/) {
  if (Window* window = Find(provider_id, serial)) {
    // The manager's own error text is not localized; the page shows none.
    window->ShowResult(ok, std::u16string_view());
  }
}

void ProviderSignInWindows::OnWindowClosed(const std::string& provider_id,
                                           uint64_t serial) {
  Window* window = Find(provider_id, serial);
  if (!window) {
    return;
  }
  if (window->device_code() && window->running()) {
    if (NexusOAuthManager* manager = OAuthManagerFor(profile_)) {
      manager->CancelDeviceCodeFlow(provider_id);
    }
  }
  // The Window is inside its own close notification; drop it afterwards.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](base::WeakPtr<ProviderSignInWindows> self,
                        std::string id, uint64_t serial) {
                       if (self && self->Find(id, serial)) {
                         self->windows_.erase(id);
                       }
                     },
                     weak_factory_.GetWeakPtr(), provider_id, serial));
}

}  // namespace nexus
