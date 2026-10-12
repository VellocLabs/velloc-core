// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_SCREENSHOT_SHAPING_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_SCREENSHOT_SHAPING_H_

#include <optional>

#include "base/values.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"

namespace nexus::browser_tool_internal {

// BrowserScreenshot image shaping: capture PNG at native resolution, then
// decode → optional crop → downscale to `kScreenshotMaxEdge` → PNG (models bill
// pixels; JPEG smears text). APC boxes and the capture share device pixels, so
// the capture must not be pre-scaled.

// Long-edge cap for the delivered image. Matches the guest's
// `TOOL_RESULT_IMAGE_MAX_EDGE` (nexus_web `utils/imagePaste.ts`), so the
// guest's own downscale becomes a no-op and never re-encodes this PNG as JPEG.
inline constexpr int kScreenshotMaxEdge = 1024;

// Context kept around a cropped element, in captured (device) pixels — enough
// to show the element's immediate surroundings (its label, a focus ring).
inline constexpr int kScreenshotCropPaddingPx = 16;

// `size` scaled to fit a `max_edge` long edge, aspect preserved. Returned
// unchanged when it already fits (never upscales). Each edge stays >= 1.
gfx::Size FitWithinMaxEdge(const gfx::Size& size, int max_edge);

// The crop rectangle for `element_box` inside an image of `image_size`:
// the box grown by `padding` on every side, clamped to the image. nullopt
// when the result is empty (the box lies entirely outside the image).
std::optional<gfx::Rect> ComputeScreenshotCropRect(const gfx::Rect& element_box,
                                                   const gfx::Size& image_size,
                                                   int padding);

// Contract: shapes `observation.tabs[].screenshot` in a BrowserScreenshot
// result; CPU-bound, off UI. `crop_index` crops to that element's box from the
// same tab. Output: `mime`, `data_base64`, `width` / `height`; `scale` when
// downscaled; `crop: {index, box}`; `crop_error` (full viewport delivered);
// `shaping_error` (original bytes kept). No screenshot → unchanged.
base::DictValue ShapeScreenshotResult(base::DictValue result,
                                      std::optional<int> crop_index,
                                      int max_edge);

}  // namespace nexus::browser_tool_internal

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_SCREENSHOT_SHAPING_H_
