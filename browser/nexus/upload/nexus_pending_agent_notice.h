// Copyright 2026 The Custom Browser Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_UPLOAD_NEXUS_PENDING_AGENT_NOTICE_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_UPLOAD_NEXUS_PENDING_AGENT_NOTICE_H_

#include <deque>
#include <string>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "base/values.h"
#include "content/public/browser/web_contents_user_data.h"
#include "third_party/blink/public/mojom/choosers/file_chooser.mojom-forward.h"

namespace content {
class FileSelectListener;
class RenderFrameHost;
class WebContents;
}  // namespace content

namespace nexus {

// One out-of-band fact the browser knows but the page can hide from the agent
// — e.g. a native file dialog that was suppressed on an agent-controlled tab.
// Serialized into the next observation's `notices[]` array so the model can
// self-correct (`recommended_tool`) instead of retrying a UI action whose
// native dialog the host silently cancelled.
struct AgentNotice {
  AgentNotice();
  AgentNotice(const AgentNotice&);
  AgentNotice(AgentNotice&&);
  AgentNotice& operator=(const AgentNotice&);
  AgentNotice& operator=(AgentNotice&&);
  ~AgentNotice();

  // Machine-readable kind, e.g. "native_dialog_suppressed". Never empty.
  std::string type;
  // Which dialog was involved, e.g. "open_file" / "directory" / "save_file".
  // Empty when not applicable.
  std::string dialog;
  // The Nexus tool the model should use instead, e.g. "BrowserUpload". Empty
  // when there is nothing for the model to redo (informational notice).
  std::string recommended_tool;
  // Model-facing English sentence. Not rendered in the transcript UI, so it
  // needs no i18n catalog key.
  std::string message;

  base::DictValue ToDict() const;
};

// Per-tab FIFO queue of pending `AgentNotice`s, attached to a
// `content::WebContents` via `WebContentsUserData`. `BrowserTool::
// BuildObservationDict` drains it into each observation, so a fact recorded
// here on the browser side reaches the model even when the page swallowed the
// corresponding JS error.
//
// Sticky until drained: a suppressed chooser fires asynchronously relative to
// the tool action that caused it, so a notice may surface on a later
// observation than the click that triggered it. Bounded (`kMaxNotices`,
// drop-oldest) so a hostile page cannot flood the queue. Auto-cleared with the
// WebContents.
class NexusPendingAgentNotice
    : public content::WebContentsUserData<NexusPendingAgentNotice> {
 public:
  // Upper bound on queued notices; the oldest is dropped once exceeded.
  static constexpr size_t kMaxNotices = 16;

  NexusPendingAgentNotice(const NexusPendingAgentNotice&) = delete;
  NexusPendingAgentNotice& operator=(const NexusPendingAgentNotice&) = delete;
  ~NexusPendingAgentNotice() override;

  // Append `notice` to `contents`'s queue, creating the queue lazily. No-op if
  // `contents` is null. Drops the oldest entry once the queue exceeds
  // `kMaxNotices`.
  static void Push(content::WebContents* contents, AgentNotice notice);

  // Move out and return every queued notice, clearing the queue. Empty vector
  // if none. One-shot: a drained notice is not returned again.
  std::vector<AgentNotice> TakeAll();

  // `Browser::RunFileChooser` hook, called after `NexusPendingUpload::
  // TryAnswerChooser` declines (nothing armed). If `render_frame_host`'s
  // WebContents is agent-owned (carries a `NexusSessionTabTag`), cancel the
  // chooser via `listener->FileSelectionCanceled()`, record a mode-keyed
  // notice, and return true — so the caller does NOT open the native dialog.
  // Otherwise returns false and leaves `listener` untouched, so a user tab
  // still gets its native dialog. Does not take ownership of `listener`.
  static bool SuppressChooserIfAgentTab(
      content::RenderFrameHost* render_frame_host,
      const scoped_refptr<content::FileSelectListener>& listener,
      const blink::mojom::FileChooserParams& params);

 private:
  friend class content::WebContentsUserData<NexusPendingAgentNotice>;
  explicit NexusPendingAgentNotice(content::WebContents* contents);

  std::deque<AgentNotice> notices_;

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_UPLOAD_NEXUS_PENDING_AGENT_NOTICE_H_
