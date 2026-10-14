// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/upload/nexus_file_chooser_interceptor.h"

#include <utility>

#include "base/logging.h"
#include "content/public/browser/file_select_listener.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "third_party/blink/public/mojom/choosers/file_chooser.mojom.h"

namespace nexus {

NexusPendingUpload::NexusPendingUpload(content::WebContents* contents)
    : content::WebContentsUserData<NexusPendingUpload>(*contents) {}

NexusPendingUpload::~NexusPendingUpload() = default;

// static
void NexusPendingUpload::Arm(content::WebContents* contents,
                             std::vector<base::FilePath> files) {
  if (!contents) {
    return;
  }
  NexusPendingUpload* pending = FromWebContents(contents);
  if (!pending) {
    CreateForWebContents(contents);
    pending = FromWebContents(contents);
  }
  pending->files_ = std::move(files);
}

// static
bool NexusPendingUpload::TryAnswerChooser(
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
  NexusPendingUpload* pending = FromWebContents(contents);
  if (!pending || pending->files_.empty()) {
    return false;
  }
  // Only answer file-open choosers. Folder enumeration (kUploadFolder) and
  // save dialogs (kSave) are out of scope — fall through to the default.
  using Mode = blink::mojom::FileChooserParams::Mode;
  if (params.mode != Mode::kOpen && params.mode != Mode::kOpenMultiple) {
    return false;
  }

  std::vector<base::FilePath> files = std::move(pending->files_);
  // One-shot: drop the armed state now. NOTE: RemoveUserData deletes
  // `pending`, so do not touch it afterwards.
  contents->RemoveUserData(UserDataKey());

  std::vector<blink::mojom::FileChooserFileInfoPtr> infos;
  for (const base::FilePath& path : files) {
    blink::mojom::NativeFileInfoPtr native =
        blink::mojom::NativeFileInfo::New();
    native->file_path = path;
    // Leave display_name empty — Blink derives File.name from the base
    // name of file_path when display_name is empty.
    infos.push_back(
        blink::mojom::FileChooserFileInfo::NewNativeFile(std::move(native)));
    // A single-file <input> only accepts one file; ignore extras.
    if (params.mode == Mode::kOpen) {
      break;
    }
  }

  LOG(INFO) << "[NexusUpload] answering file chooser with " << infos.size()
            << " staged file(s) (mode="
            << static_cast<int>(params.mode) << ")";
  listener->FileSelected(std::move(infos), /*base_dir=*/base::FilePath(),
                         params.mode);
  return true;
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(NexusPendingUpload);

}  // namespace nexus
