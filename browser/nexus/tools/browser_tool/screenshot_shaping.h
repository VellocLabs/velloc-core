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

// BrowserScreenshot image shaping — the step between the upstream capture and
// the model.
//
// Why this exists. A vision model bills an image by its PIXEL COUNT, not its
// byte size, so the two levers that actually move cost and legibility are:
//   * fewer pixels — cap the long edge, or crop to the one element the model
//     asked about;
//   * clean glyph edges — screenshots are mostly text, and a lossy JPEG
//     (upstream's default is quality 40) smears exactly the high-frequency
//     strokes a model needs to tell `l`/`1`/`I` apart.
// So the capture is requested as PNG at native resolution and shaped here:
// decode → optional crop → downscale to `kScreenshotMaxEdge` → PNG.
//
// Coordinate space. APC element boxes (`elements[].box`) are in Blink space,
// i.e. device pixels, and a native-resolution viewport capture is in the same
// device pixels — so a box maps onto the captured image 1:1 with no DPR
// arithmetic. That is why the capture must NOT be pre-scaled upstream
// (`max_width`/`max_height` stay unset): the crop has to happen before the
// downscale.

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

// Shapes the screenshot carried by a BrowserScreenshot tool result
// (`observation.tabs[].screenshot = {mime, data_base64}`), in place of the
// returned copy. CPU-bound (decode/resize/encode) — run it off the UI thread.
//
// With `crop_index`, the image is cropped to that element's `box` from the
// SAME tab's `elements[]` (the fresh walk taken with this capture, so the box
// and the pixels agree). Every outcome is explicit in the screenshot dict:
//   * `mime` "image/png", `data_base64`, `width` / `height` (delivered pixels);
//   * `scale` — delivered pixels per observation pixel, present only when the
//     image was downscaled (map an image point back: box_px = crop.x + px /
//     scale);
//   * `crop: {index, box:[x,y,w,h]}` — the region actually delivered, in
//     observation coordinates;
//   * `crop_error` — the index had no usable box; the FULL viewport was
//     delivered instead (never a silent fallback);
//   * `shaping_error` — the capture could not be decoded/encoded; the original
//     bytes are passed through untouched.
// A result without a screenshot is returned unchanged.
base::DictValue ShapeScreenshotResult(base::DictValue result,
                                      std::optional<int> crop_index,
                                      int max_edge);

}  // namespace nexus::browser_tool_internal

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_SCREENSHOT_SHAPING_H_
