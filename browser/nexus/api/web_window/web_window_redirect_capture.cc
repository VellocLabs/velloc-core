// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/web_window/web_window_redirect_capture.h"

#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/sequenced_task_runner.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents.h"
#include "custom_browser/browser/nexus/api/web_window/web_window_options.h"

namespace nexus {

namespace {

const char kUserDataKey[] = "nexus_web_window_redirect_capture";

}  // namespace

// static
void WebWindowRedirectCapture::Attach(content::WebContents* contents,
                                      GURL target,
                                      CaptureCallback on_capture) {
  contents->SetUserData(kUserDataKey,
                        std::make_unique<WebWindowRedirectCapture>(
                            std::move(target), std::move(on_capture)));
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
  if (!marker || !MatchesWebWindowCaptureTarget(marker->target_,
                                                handle->GetURL())) {
    return false;
  }
  CaptureCallback on_capture = std::move(marker->on_capture_);
  contents->RemoveUserData(kUserDataKey);  // Deletes `marker`.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(std::move(on_capture), handle->GetURL()));
  return true;
}

WebWindowRedirectCapture::WebWindowRedirectCapture(GURL target,
                                                   CaptureCallback on_capture)
    : target_(std::move(target)), on_capture_(std::move(on_capture)) {}

WebWindowRedirectCapture::~WebWindowRedirectCapture() = default;

}  // namespace nexus
