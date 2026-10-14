// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/utility/nexus/media/media_read_result.h"

#include <utility>

#include "mojo/public/cpp/base/big_buffer.h"
#include "base/containers/span.h"

namespace nexus {

MediaReadOutcome::MediaReadOutcome() = default;
MediaReadOutcome::MediaReadOutcome(MediaReadOutcome&&) = default;
MediaReadOutcome& MediaReadOutcome::operator=(MediaReadOutcome&&) = default;
MediaReadOutcome::~MediaReadOutcome() = default;

mojom::MediaPartPtr MakeTextMediaPart(std::string text) {
  auto part = mojom::MediaPart::New();
  part->type = "text";
  part->text = std::move(text);
  return part;
}

mojom::MediaPartPtr MakeBinaryMediaPart(std::string type,
                                        std::string mime_type,
                                        std::vector<uint8_t> data,
                                        std::optional<uint32_t> page_number) {
  auto part = mojom::MediaPart::New();
  part->type = std::move(type);
  part->mime_type = std::move(mime_type);
  part->data = mojo_base::BigBuffer(base::span(data));
  part->page_number = page_number;
  return part;
}

MediaReadOutcome MakeMediaError(std::string code, std::string message) {
  MediaReadOutcome outcome;
  outcome.error_code = std::move(code);
  outcome.error_message = std::move(message);
  return outcome;
}

}  // namespace nexus
