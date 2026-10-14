// Copyright 2026 VellocLabs
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_UTILITY_NEXUS_MEDIA_MEDIA_LIMITS_H_
#define CUSTOM_BROWSER_UTILITY_NEXUS_MEDIA_MEDIA_LIMITS_H_

#include <stddef.h>

namespace nexus::media_limits {

inline constexpr size_t kMediaReadByteCap = 12 * 1024 * 1024;
inline constexpr size_t kReadResponseByteCap = 4 * 1024 * 1024;
inline constexpr size_t kImageCompressionThreshold = 256 * 1024;
inline constexpr int kImageMaxDimension = 1600;
inline constexpr int kPdfInlinePageLimit = 10;
inline constexpr int kPdfMaxPagesPerRead = 20;
inline constexpr int kPdfRenderDpi = 100;
inline constexpr int kPdfRenderMaxDimension = 1800;
inline constexpr size_t kNotebookOutputThreshold = 10'000;

}  // namespace nexus::media_limits

#endif  // CUSTOM_BROWSER_UTILITY_NEXUS_MEDIA_MEDIA_LIMITS_H_
