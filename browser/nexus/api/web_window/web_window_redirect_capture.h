// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_WEB_WINDOW_WEB_WINDOW_REDIRECT_CAPTURE_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_WEB_WINDOW_WEB_WINDOW_REDIRECT_CAPTURE_H_

#include "base/functional/callback.h"
#include "base/supports_user_data.h"
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
// Contract: UI thread only. At most one capture per marker — the marker is
// removed when it fires. `on_capture` runs in a later task, never inside the
// navigation throttle, so it may close the window. Bind it to a WeakPtr: the
// marker can outlive whoever attached it.
class WebWindowRedirectCapture : public base::SupportsUserData::Data {
 public:
  using CaptureCallback = base::OnceCallback<void(const GURL& url)>;

  // Arms `contents`, replacing any earlier marker. `target` must satisfy
  // IsValidWebWindowCaptureTarget.
  static void Attach(content::WebContents* contents,
                     GURL target,
                     CaptureCallback on_capture);

  // Called by the navigation throttle at request start and at every
  // redirect. True when `handle` is a primary-main-frame navigation to the
  // armed target: the caller must cancel it (no request may reach the
  // loopback port), and `on_capture` is posted with the full URL.
  static bool MaybeCapture(content::NavigationHandle* handle);

  WebWindowRedirectCapture(GURL target, CaptureCallback on_capture);
  ~WebWindowRedirectCapture() override;

  WebWindowRedirectCapture(const WebWindowRedirectCapture&) = delete;
  WebWindowRedirectCapture& operator=(const WebWindowRedirectCapture&) =
      delete;

 private:
  const GURL target_;
  CaptureCallback on_capture_;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_WEB_WINDOW_WEB_WINDOW_REDIRECT_CAPTURE_H_
