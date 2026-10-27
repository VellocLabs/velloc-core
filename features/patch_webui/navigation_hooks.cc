#include "custom_browser/features/patch_webui/navigation_hooks.h"

#include <memory>
#include <string>

#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/task/single_thread_task_runner.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/search/search.h"
#include "chrome/common/webui_url_constants.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/navigation_throttle.h"
#include "content/public/browser/web_contents.h"
#include "custom_browser/browser/nexus/api/web_window/web_window_redirect_capture.h"
#include "ui/base/page_transition_types.h"
#include "url/gurl.h"

namespace custom_browser::patch_webui {

namespace {

void NavigateToNewTab(base::WeakPtr<content::WebContents> web_contents,
                      GURL target_url) {
  if (!web_contents || !target_url.is_valid()) {
    return;
  }

  content::NavigationController::LoadURLParams params(target_url);
  params.transition_type = ui::PAGE_TRANSITION_FROM_ADDRESS_BAR;
  params.is_renderer_initiated = false;
  web_contents->GetController().LoadURLWithParams(params);
}

class SearchRedirectThrottle : public content::NavigationThrottle {
 public:
  explicit SearchRedirectThrottle(content::NavigationThrottleRegistry& registry)
      : content::NavigationThrottle(registry) {}
  ~SearchRedirectThrottle() override = default;

  const char* GetNameForLogging() override {
    return "CustomBrowserSearchRedirectThrottle";
  }

  ThrottleCheckResult WillStartRequest() override {
    return MaybeRedirectToNewTab();
  }

  ThrottleCheckResult WillRedirectRequest() override {
    return MaybeRedirectToNewTab();
  }

 private:
  ThrottleCheckResult MaybeRedirectToNewTab() {
    content::NavigationHandle* handle = navigation_handle();
    if (!handle->IsInMainFrame() || handle->IsSameDocument()) {
      return PROCEED;
    }

    const GURL& url = handle->GetURL();
    if (!url.SchemeIsHTTPOrHTTPS()) {
      return PROCEED;
    }

    content::WebContents* web_contents = handle->GetWebContents();
    if (!web_contents) {
      return PROCEED;
    }

    Profile* profile =
        Profile::FromBrowserContext(web_contents->GetBrowserContext());
    if (!profile || profile->IsOffTheRecord()) {
      return PROCEED;
    }

    if (search::GetNewTabPageURL(profile) != url || search::DefaultSearchProviderIsGoogle(profile)) {
      return PROCEED;
    }

    GURL target_url(chrome::kChromeUINewTabPageURL);
    base::WeakPtr<content::WebContents> weak_contents =
        web_contents->GetWeakPtr();
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&NavigateToNewTab, std::move(weak_contents),
                       std::move(target_url)));
    return CANCEL_AND_IGNORE;
  }
};

// Cancels a single-web window's armed `captureRedirect` navigation (F-29)
// before any request reaches the loopback port; the capture is reported to
// whoever armed the window.
class NexusOAuthRedirectThrottle : public content::NavigationThrottle {
 public:
  explicit NexusOAuthRedirectThrottle(
      content::NavigationThrottleRegistry& registry)
      : content::NavigationThrottle(registry) {}
  ~NexusOAuthRedirectThrottle() override = default;

  const char* GetNameForLogging() override {
    return "NexusOAuthRedirectThrottle";
  }

  ThrottleCheckResult WillStartRequest() override { return MaybeCapture(); }
  ThrottleCheckResult WillRedirectRequest() override { return MaybeCapture(); }

 private:
  ThrottleCheckResult MaybeCapture() {
    return nexus::WebWindowRedirectCapture::MaybeCapture(navigation_handle())
               ? CANCEL_AND_IGNORE
               : PROCEED;
  }
};

}  // namespace

void AddSearchRedirectThrottle(
    content::NavigationThrottleRegistry& registry) {
  if (!registry.GetNavigationHandle().GetURL().SchemeIsHTTPOrHTTPS()) {
    return;
  }

  registry.AddThrottle(
      std::make_unique<SearchRedirectThrottle>(registry));
}

void AddNexusOAuthRedirectThrottle(
    content::NavigationThrottleRegistry& registry) {
  if (!registry.GetNavigationHandle().GetURL().SchemeIsHTTPOrHTTPS()) {
    return;
  }

  registry.AddThrottle(
      std::make_unique<NexusOAuthRedirectThrottle>(registry));
}

}  // namespace custom_browser::patch_webui
