// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_UTILITY_NEXUS_MEDIA_MEDIA_READ_RESULT_H_
#define CUSTOM_BROWSER_UTILITY_NEXUS_MEDIA_MEDIA_READ_RESULT_H_

#include <optional>
#include <string>
#include <vector>

#include "custom_browser/browser/nexus/mojom/nexus_media.mojom.h"

namespace nexus {

struct MediaReadOutcome {
  MediaReadOutcome();
  MediaReadOutcome(MediaReadOutcome&&);
  MediaReadOutcome& operator=(MediaReadOutcome&&);
  MediaReadOutcome(const MediaReadOutcome&) = delete;
  MediaReadOutcome& operator=(const MediaReadOutcome&) = delete;
  ~MediaReadOutcome();

  bool ok = false;
  mojom::MediaReadResultPtr result;
  std::optional<std::string> error_code;
  std::optional<std::string> error_message;
};

mojom::MediaPartPtr MakeTextMediaPart(std::string text);
mojom::MediaPartPtr MakeBinaryMediaPart(std::string type,
                                        std::string mime_type,
                                        std::vector<uint8_t> data,
                                        std::optional<uint32_t> page_number = std::nullopt);
MediaReadOutcome MakeMediaError(std::string code, std::string message);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_UTILITY_NEXUS_MEDIA_MEDIA_READ_RESULT_H_
