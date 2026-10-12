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

// Per-tab "armed upload": the file path(s) the agent staged for the NEXT file
// chooser `contents` opens. A guarded hook in `Browser::RunFileChooser` calls
// `TryAnswerChooser()`, which answers a file-open chooser with them (no native
// dialog); otherwise the normal dialog runs. One-shot; cleared with the tab.
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
