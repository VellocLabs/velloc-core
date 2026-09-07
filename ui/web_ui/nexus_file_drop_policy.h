#ifndef CUSTOM_BROWSER_UI_WEB_UI_NEXUS_FILE_DROP_POLICY_H_
#define CUSTOM_BROWSER_UI_WEB_UI_NEXUS_FILE_DROP_POLICY_H_

#include "content/public/common/drop_data.h"

namespace custom_browser {

// Whether an OS drag carries files — the only external drag the kNexus
// window's page admits.
//
// Why it exists: `CustomBrowserWebUIView` is the WebContentsDelegate of the
// kNexus window's page, and the Nexus guest is a <webview> inside that page.
// WebContentsViewAura asks the OUTER contents' delegate `CanDragEnter` for
// every drag, including one aimed at the guest, and the delegate was written
// for the WebUI tab strip: it admitted browser-tab drags only. Every file
// dragged from Explorer onto the composer was therefore refused before the
// guest's dropzone saw it (a "no-drop" cursor, whatever the model). Upstream
// Glic, the same <webview>-in-WebUI shape, admits files the same way
// (GlicView::CanDragEnter). The view must also report itself unprivileged in
// the kNexus window (CustomBrowserWebUIView::IsPrivileged): an OS drag is
// never privileged, and the privilege match is checked first.
//
// Files only: the guest adds attachments from `DataTransfer.files`, so text,
// HTML or a bare URL has nothing to become. A dropped file never navigates:
// guests refuse load drops, and the kNexus page turns them off for itself
// (CustomBrowserWebUIView::InitializeWebView).
inline bool IsNexusFileDrop(const content::DropData& data) {
  return !data.filenames.empty() || !data.file_system_files.empty();
}

}  // namespace custom_browser

#endif  // CUSTOM_BROWSER_UI_WEB_UI_NEXUS_FILE_DROP_POLICY_H_
