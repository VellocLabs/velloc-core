// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_WEB_WINDOW_WEB_WINDOW_REDIRECT_CAPTURE_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_WEB_WINDOW_WEB_WINDOW_REDIRECT_CAPTURE_H_

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/supports_user_data.h"
#include "content/public/browser/web_contents_observer.h"
#include "url/gurl.h"

namespace content {
class NavigationHandle;
class WebContents;
}  // namespace content

namespace nexus {

// The `captureRedirect` marker of a single-web window (F-29): a WebContents
// user-data entry naming the loopback URL whose navigation is swallowed and
// reported instead of sent.
//
// Contract: UI thread only. A page the armed contents opens later (a popup
// window or a new tab, with or without an opener) is armed too, and so on
// down; all of them share one capture, which fires once — the first match in
// any of them removes the marker there and disarms the others. `on_capture`
// runs in a later task, never inside the navigation throttle, so it may close
// the window. Bind it to a WeakPtr: the marker can outlive whoever attached
// it.
//
// Security: arming the opened pages keeps a sign-in page that finishes in a
// popup from sending the redirect to whatever listens on the loopback port.
class WebWindowRedirectCapture : public base::SupportsUserData::Data,
                                 public content::WebContentsObserver {
 public:
  using CaptureCallback = base::OnceCallback<void(const GURL& url)>;

  // Arms `contents`, replacing any earlier marker. `target` must satisfy
  // IsValidWebWindowCaptureTarget.
  static void Attach(content::WebContents* contents,
                     GURL target,
                     CaptureCallback on_capture);

  // Whether `contents` carries a marker that has not fired.
  static bool IsArmed(content::WebContents* contents);

  // Called by the navigation throttle at request start and at every
  // redirect. True when `handle` is a primary-main-frame navigation to the
  // armed target: the caller must cancel it (no request may reach the
  // loopback port), and `on_capture` is posted with the full URL.
  static bool MaybeCapture(content::NavigationHandle* handle);

  WebWindowRedirectCapture(const WebWindowRedirectCapture&) = delete;
  WebWindowRedirectCapture& operator=(const WebWindowRedirectCapture&) =
      delete;
  ~WebWindowRedirectCapture() override;

 private:
  class Capture;

  WebWindowRedirectCapture(content::WebContents* contents,
                           scoped_refptr<Capture> capture);

  static void AttachShared(content::WebContents* contents,
                           scoped_refptr<Capture> capture);

  // content::WebContentsObserver:
  void DidOpenRequestedURL(content::WebContents* new_contents,
                           content::RenderFrameHost* source_render_frame_host,
                           const GURL& url,
                           const content::Referrer& referrer,
                           WindowOpenDisposition disposition,
                           ui::PageTransition transition,
                           bool started_from_context_menu,
                           bool renderer_initiated) override;

  const scoped_refptr<Capture> capture_;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_WEB_WINDOW_WEB_WINDOW_REDIRECT_CAPTURE_H_
