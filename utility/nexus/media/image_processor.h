// Copyright 2026 VellocLabs
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_UTILITY_NEXUS_MEDIA_IMAGE_PROCESSOR_H_
#define CUSTOM_BROWSER_UTILITY_NEXUS_MEDIA_IMAGE_PROCESSOR_H_

#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "custom_browser/utility/nexus/media/media_read_result.h"

namespace nexus {

MediaReadOutcome ReadImageMedia(const base::FilePath& path,
                                const std::string& mime_type);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_UTILITY_NEXUS_MEDIA_IMAGE_PROCESSOR_H_
