// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/web_window/web_window_redirect_capture.h"

#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/ptr_util.h"
#include "base/memory/ref_counted.h"
#include "base/task/sequenced_task_runner.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents.h"
#include "custom_browser/browser/nexus/api/web_window/web_window_options.h"

namespace nexus {

namespace {

const char kUserDataKey[] = "nexus_web_window_redirect_capture";

}  // namespace

// The capture every page of one armed window shares.
class WebWindowRedirectCapture::Capture
    : public base::RefCounted<WebWindowRedirectCapture::Capture> {
 public:
  Capture(GURL target, CaptureCallback on_capture)
      : target_(std::move(target)), on_capture_(std::move(on_capture)) {}

  Capture(const Capture&) = delete;
  Capture& operator=(const Capture&) = delete;

  const GURL& target() const { return target_; }
  bool armed() const { return !on_capture_.is_null(); }
  CaptureCallback Take() { return std::move(on_capture_); }

 private:
  friend class base::RefCounted<Capture>;
  ~Capture() = default;

  const GURL target_;
  CaptureCallback on_capture_;
};

// static
void WebWindowRedirectCapture::Attach(content::WebContents* contents,
                                      GURL target,
                                      CaptureCallback on_capture) {
  AttachShared(contents, base::MakeRefCounted<Capture>(std::move(target),
                                                       std::move(on_capture)));
}

// static
void WebWindowRedirectCapture::AttachShared(content::WebContents* contents,
                                            scoped_refptr<Capture> capture) {
  contents->SetUserData(kUserDataKey,
                        base::WrapUnique(new WebWindowRedirectCapture(
                            contents, std::move(capture))));
}

// static
bool WebWindowRedirectCapture::IsArmed(content::WebContents* contents) {
  auto* marker = static_cast<WebWindowRedirectCapture*>(
      contents ? contents->GetUserData(kUserDataKey) : nullptr);
  return marker && marker->capture_->armed();
}

// static
bool WebWindowRedirectCapture::MaybeCapture(
    content::NavigationHandle* handle) {
  if (!handle->IsInPrimaryMainFrame() || handle->IsSameDocument()) {
    return false;
  }
  content::WebContents* contents = handle->GetWebContents();
  if (!contents) {
    return false;
  }
  auto* marker = static_cast<WebWindowRedirectCapture*>(
      contents->GetUserData(kUserDataKey));
  if (!marker || !marker->capture_->armed() ||
      !MatchesWebWindowCaptureTarget(marker->capture_->target(),
                                     handle->GetURL())) {
    return false;
  }
  CaptureCallback on_capture = marker->capture_->Take();
  contents->RemoveUserData(kUserDataKey);  // Deletes `marker`.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(std::move(on_capture), handle->GetURL()));
  return true;
}

WebWindowRedirectCapture::WebWindowRedirectCapture(
    content::WebContents* contents,
    scoped_refptr<Capture> capture)
    : content::WebContentsObserver(contents), capture_(std::move(capture)) {}

WebWindowRedirectCapture::~WebWindowRedirectCapture() = default;

void WebWindowRedirectCapture::DidOpenRequestedURL(
    content::WebContents* new_contents,
    content::RenderFrameHost* /*source_render_frame_host*/,
    const GURL& /*url*/,
    const content::Referrer& /*referrer*/,
    WindowOpenDisposition /*disposition*/,
    ui::PageTransition /*transition*/,
    bool /*started_from_context_menu*/,
    bool /*renderer_initiated*/) {
  if (new_contents && new_contents != web_contents() && capture_->armed()) {
    AttachShared(new_contents, capture_);
  }
}

}  // namespace nexus
