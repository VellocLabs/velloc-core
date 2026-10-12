#ifndef CUSTOM_BROWSER_UI_WEB_UI_NEXUS_FILE_DROP_POLICY_H_
#define CUSTOM_BROWSER_UI_WEB_UI_NEXUS_FILE_DROP_POLICY_H_

#include "content/public/common/drop_data.h"

namespace custom_browser {

// Whether an OS drag carries files, the only external drag the kNexus window's
// page admits (the outer delegate's `CanDragEnter` also sees drags aimed at the
// guest). Files only: text, HTML or a bare URL is refused. A dropped file never
// navigates: guests refuse load drops and the kNexus page disables them.
inline bool IsNexusFileDrop(const content::DropData& data) {
  return !data.filenames.empty() || !data.file_system_files.empty();
}

}  // namespace custom_browser

#endif  // CUSTOM_BROWSER_UI_WEB_UI_NEXUS_FILE_DROP_POLICY_H_
