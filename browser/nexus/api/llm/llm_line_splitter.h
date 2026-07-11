// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_LINE_SPLITTER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_LINE_SPLITTER_H_

#include <string>
#include <string_view>
#include <vector>

namespace nexus {

// Incremental line splitter used by provider stream parsers to turn an
// arbitrary stream of bytes (SSE, NDJSON, …) into whole lines.
//
// Bytes arrive in chunks of any size; a single logical line may straddle
// chunks. Feed() accumulates an internal buffer, extracts every complete
// line (terminated by "\n" or "\r\n"), strips the terminator, and returns
// them. Any trailing incomplete partial line stays buffered for the next
// Feed().
//
// Empty lines are emitted as empty strings — callers that want to ignore
// blank lines (SSE comment handling, etc.) must filter themselves; this
// class exists to own buffer lifetime and newline scanning, not protocol
// semantics.
//
// Usage:
//   LineSplitter splitter;
//   for (each chunk of bytes) {
//     for (const auto& line : splitter.Feed(chunk)) {
//       HandleLine(line);
//     }
//   }
//   // At stream end, if a terminator-less trailing line is meaningful:
//   if (auto tail = splitter.DrainRemaining(); !tail.empty()) {
//     HandleLine(tail);
//   }
class LineSplitter {
 public:
  LineSplitter();
  ~LineSplitter();

  LineSplitter(const LineSplitter&) = delete;
  LineSplitter& operator=(const LineSplitter&) = delete;

  // Appends |bytes| to the internal buffer and returns every newly completed
  // line. A trailing "\r" is stripped before the line is returned, so callers
  // get canonical LF-terminated semantics on CRLF input.
  std::vector<std::string> Feed(std::string_view bytes);

  // Returns and clears any trailing un-terminated bytes in the buffer.
  // Empty string if the buffer has nothing left (last byte was a newline).
  // Note: a trailing "\r" is NOT stripped here — DrainRemaining is for the
  // rare "server closed without a final newline" case where the raw tail
  // is what the caller wants.
  std::string DrainRemaining();

  // True when no un-terminated bytes are waiting.
  bool empty() const { return buffer_.empty(); }

 private:
  std::string buffer_;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_LINE_SPLITTER_H_
