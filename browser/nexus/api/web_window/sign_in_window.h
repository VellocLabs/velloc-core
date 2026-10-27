// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_WEB_WINDOW_SIGN_IN_WINDOW_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_WEB_WINDOW_SIGN_IN_WINDOW_H_

#include <string>
#include <string_view>

#include "base/functional/callback.h"
#include "base/timer/timer.h"
#include "content/public/browser/web_contents_observer.h"
#include "custom_browser/browser/nexus/api/web_window/sign_in_status_page.h"
#include "url/gurl.h"

namespace content {
class RenderFrameHost;
class WebContents;
}  // namespace content

namespace nexus {

// The status-page text in the browser's UI language.
SignInStatusText LocalizedSignInStatusText();

// "Sign in to {service_name}", the fixed title of a sign-in window.
std::u16string SignInWindowTitle(std::u16string_view service_name);

// Replaces the page in `contents` with the host's status page for `status`
// (F-29) and returns the page's URL. Contract: UI thread; null `contents` is
// a no-op that returns an empty URL. The load is browser-initiated and
// replaces the current history entry, so Back does not return to the status
// page.
GURL ShowSignInStatus(content::WebContents* contents,
                      SignInStatus status,
                      std::u16string_view reason = {});

// Runs `close` kSignInSuccessHold after `page` finished (or failed) loading
// in the primary main frame of `contents`, so the success page shows for the
// whole hold however long its load took. Contract: UI thread; `close` runs at
// most once, and never after this object or `contents` is destroyed.
class SignInSuccessCloser : public content::WebContentsObserver {
 public:
  SignInSuccessCloser(content::WebContents* contents,
                      GURL page,
                      base::OnceClosure close);
  ~SignInSuccessCloser() override;

  SignInSuccessCloser(const SignInSuccessCloser&) = delete;
  SignInSuccessCloser& operator=(const SignInSuccessCloser&) = delete;

 private:
  // content::WebContentsObserver:
  void DidFinishLoad(content::RenderFrameHost* render_frame_host,
                     const GURL& validated_url) override;
  void DidFailLoad(content::RenderFrameHost* render_frame_host,
                   const GURL& validated_url,
                   int error_code) override;
  void WebContentsDestroyed() override;

  void StartHold(content::RenderFrameHost* render_frame_host,
                 const GURL& url);

  const GURL page_;
  base::OnceClosure close_;
  base::OneShotTimer hold_;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_WEB_WINDOW_SIGN_IN_WINDOW_H_
