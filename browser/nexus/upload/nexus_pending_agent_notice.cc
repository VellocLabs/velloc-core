// Copyright 2026 The Custom Browser Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/upload/nexus_pending_agent_notice.h"

#include <iterator>
#include <utility>

#include "base/logging.h"
#include "content/public/browser/file_select_listener.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "custom_browser/browser/nexus/session/nexus_session_tab_tag.h"
#include "third_party/blink/public/mojom/choosers/file_chooser.mojom.h"

namespace nexus {

AgentNotice::AgentNotice() = default;
AgentNotice::AgentNotice(const AgentNotice&) = default;
AgentNotice::AgentNotice(AgentNotice&&) = default;
AgentNotice& AgentNotice::operator=(const AgentNotice&) = default;
AgentNotice& AgentNotice::operator=(AgentNotice&&) = default;
AgentNotice::~AgentNotice() = default;

base::DictValue AgentNotice::ToDict() const {
  base::DictValue dict;
  dict.Set("type", type);
  if (!dialog.empty()) {
    dict.Set("dialog", dialog);
  }
  if (!recommended_tool.empty()) {
    dict.Set("recommended_tool", recommended_tool);
  }
  dict.Set("message", message);
  return dict;
}

NexusPendingAgentNotice::NexusPendingAgentNotice(
    content::WebContents* contents)
    : content::WebContentsUserData<NexusPendingAgentNotice>(*contents) {}

NexusPendingAgentNotice::~NexusPendingAgentNotice() = default;

// static
void NexusPendingAgentNotice::Push(content::WebContents* contents,
                                   AgentNotice notice) {
  if (!contents) {
    return;
  }
  NexusPendingAgentNotice* pending = FromWebContents(contents);
  if (!pending) {
    CreateForWebContents(contents);
    pending = FromWebContents(contents);
  }
  pending->notices_.push_back(std::move(notice));
  // Drop-oldest once the queue is over the cap — a hostile page must not be
  // able to grow it without bound.
  while (pending->notices_.size() > kMaxNotices) {
    pending->notices_.pop_front();
  }
}

std::vector<AgentNotice> NexusPendingAgentNotice::TakeAll() {
  std::vector<AgentNotice> out(std::make_move_iterator(notices_.begin()),
                               std::make_move_iterator(notices_.end()));
  notices_.clear();
  return out;
}

// static
bool NexusPendingAgentNotice::SuppressChooserIfAgentTab(
    content::RenderFrameHost* render_frame_host,
    const scoped_refptr<content::FileSelectListener>& listener,
    const blink::mojom::FileChooserParams& params) {
  if (!render_frame_host || !listener) {
    return false;
  }
  content::WebContents* contents =
      content::WebContents::FromRenderFrameHost(render_frame_host);
  if (!contents) {
    return false;
  }
  // Only agent-owned tabs. The user's own tabs keep the native dialog — a
  // human may legitimately be uploading a file there.
  if (!NexusSessionTabTag::SessionKeyFor(contents)) {
    return false;
  }

  using Mode = blink::mojom::FileChooserParams::Mode;
  AgentNotice notice;
  notice.type = "native_dialog_suppressed";
  switch (params.mode) {
    case Mode::kOpen:
    case Mode::kOpenMultiple:
      notice.dialog = "open_file";
      notice.recommended_tool = "BrowserUpload";
      notice.message =
          "A native file-open dialog was suppressed on this agent-controlled "
          "tab. Use BrowserUpload{index, file_path} to attach a local file "
          "instead.";
      break;
    case Mode::kUploadFolder:
    case Mode::kOpenDirectory:
      notice.dialog = "directory";
      notice.message =
          "A native folder picker was suppressed on this agent-controlled "
          "tab. Agent tabs cannot enumerate local folders.";
      break;
    case Mode::kSave:
      notice.dialog = "save_file";
      notice.recommended_tool = "BrowserDownload";
      notice.message =
          "A native save-file dialog was suppressed on this agent-controlled "
          "tab. Use BrowserDownload to capture files instead.";
      break;
  }

  LOG(INFO) << "[NexusUpload] suppressing native file chooser on agent tab "
               "(mode="
            << static_cast<int>(params.mode) << ")";
  listener->FileSelectionCanceled();
  Push(contents, std::move(notice));
  return true;
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(NexusPendingAgentNotice);

}  // namespace nexus
