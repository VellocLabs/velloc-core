// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/llm/llm_line_splitter.h"

#include <utility>

namespace nexus {

LineSplitter::LineSplitter() = default;
LineSplitter::~LineSplitter() = default;

std::vector<std::string> LineSplitter::Feed(std::string_view bytes) {
  buffer_.append(bytes.data(), bytes.size());

  std::vector<std::string> lines;
  size_t pos = 0;
  size_t newline_pos;
  while ((newline_pos = buffer_.find('\n', pos)) != std::string::npos) {
    size_t end = newline_pos;
    // Strip a trailing CR so callers don't see CRLF residue.
    if (end > pos && buffer_[end - 1] == '\r') {
      --end;
    }
    lines.emplace_back(buffer_, pos, end - pos);
    pos = newline_pos + 1;
  }

  if (pos > 0) {
    buffer_.erase(0, pos);
  }
  return lines;
}

std::string LineSplitter::DrainRemaining() {
  std::string out = std::move(buffer_);
  buffer_.clear();
  return out;
}

}  // namespace nexus
