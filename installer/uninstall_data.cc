// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/installer/uninstall_data.h"

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/logging.h"
#include "custom_browser/common/nexus_runtime_paths.h"

namespace custom_browser::installer {

bool DeleteNexusRuntimeData() {
  const base::FilePath dir = GetNexusRuntimeDir();
  if (dir.empty()) {
    LOG(ERROR) << "No home directory; leaving the Nexus runtime data alone.";
    return false;
  }

  // base::DeletePathRecursively never traverses a link
  // (base::features::kPreventReparsePointTraversal, enabled by default): a
  // junction, inside ~/.velloc or ~/.velloc itself, goes as a link and its
  // target stays. uninstall_data_unittest.cc pins both cases.
  VLOG(1) << "Deleting the Nexus runtime data " << dir.value();
  if (!base::DeletePathRecursively(dir)) {
    LOG(ERROR) << "Failed to delete " << dir.value();
    return false;
  }
  return true;
}

}  // namespace custom_browser::installer
