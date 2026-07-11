// Copyright 2026 The Custom Browser Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_UPLOAD_NEXUS_FILE_CHOOSER_INTERCEPTOR_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_UPLOAD_NEXUS_FILE_CHOOSER_INTERCEPTOR_H_

#include <vector>

#include "base/files/file_path.h"
#include "base/memory/scoped_refptr.h"
#include "content/public/browser/web_contents_user_data.h"
#include "third_party/blink/public/mojom/choosers/file_chooser.mojom-forward.h"

namespace content {
class FileSelectListener;
class RenderFrameHost;
class WebContents;
}  // namespace content

namespace nexus {

// Per-tab "armed upload": the local file path(s) the Nexus agent staged to
// satisfy the NEXT `<input type=file>` chooser opened by `contents`.
//
// Flow: `BrowserUpload` (RunUploadOnUI) calls `Arm()` and then clicks the
// file input via the actor; the click makes Blink ask the browser to open
// a file chooser, which reaches the `ENABLE_CUSTOM_BROWSER`-guarded hook in
// `Browser::RunFileChooser` (chrome/browser/ui/browser.cc). That hook calls
// `TryAnswerChooser()`, which — if this tab has an armed upload and the
// chooser is a file-open mode — answers the chooser directly via
// `content::FileSelectListener::FileSelected(...)` with the staged files
// (no native dialog) and clears the armed state. If nothing is armed, the
// hook falls through to the normal `FileSelectHelper` (native dialog).
//
// CDP-free by design: this is the in-process file-chooser path the browser
// already uses for human picks, so it carries no DevTools-detectable
// fingerprint. One-shot: the armed set is consumed on the first matching
// chooser and auto-cleared with the WebContents.
class NexusPendingUpload
    : public content::WebContentsUserData<NexusPendingUpload> {
 public:
  NexusPendingUpload(const NexusPendingUpload&) = delete;
  NexusPendingUpload& operator=(const NexusPendingUpload&) = delete;
  ~NexusPendingUpload() override;

  // Stage `files` to answer the next file-open chooser on `contents`.
  // Replaces any previously-staged set. No-op if `contents` is null.
  static void Arm(content::WebContents* contents,
                  std::vector<base::FilePath> files);

  // Hook entry point, called from `Browser::RunFileChooser`. Returns true
  // (and answers `listener`) iff `render_frame_host`'s WebContents has an
  // armed upload AND `params.mode` is a file-open mode (kOpen /
  // kOpenMultiple). Otherwise returns false and leaves `listener` untouched
  // so the caller shows the native dialog. Does not take ownership of
  // `listener` (the listener is ref-counted; we only invoke a method).
  static bool TryAnswerChooser(
      content::RenderFrameHost* render_frame_host,
      const scoped_refptr<content::FileSelectListener>& listener,
      const blink::mojom::FileChooserParams& params);

 private:
  friend class content::WebContentsUserData<NexusPendingUpload>;
  explicit NexusPendingUpload(content::WebContents* contents);

  std::vector<base::FilePath> files_;

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_UPLOAD_NEXUS_FILE_CHOOSER_INTERCEPTOR_H_
