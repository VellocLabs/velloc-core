// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_UTILITY_NEXUS_MEDIA_PDF_PROCESSOR_H_
#define CUSTOM_BROWSER_UTILITY_NEXUS_MEDIA_PDF_PROCESSOR_H_

#include <optional>
#include <string>

#include "base/files/file_path.h"
#include "custom_browser/utility/nexus/media/media_read_result.h"

namespace nexus {

MediaReadOutcome ReadPdfMedia(base::FilePath path,
                              std::optional<std::string> pages);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_UTILITY_NEXUS_MEDIA_PDF_PROCESSOR_H_
