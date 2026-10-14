// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/tools/browser_tool/screenshot_shaping.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "skia/ext/image_operations.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkRect.h"
#include "ui/gfx/codec/jpeg_codec.h"
#include "ui/gfx/codec/png_codec.h"

namespace nexus::browser_tool_internal {

namespace {

constexpr char kPngMime[] = "image/png";

SkBitmap DecodeCapture(const std::vector<uint8_t>& bytes,
                       const std::string& mime) {
  // Upstream sets the mime from the format it actually encoded, so trust it
  // first — but fall back to the other codec rather than fail on a mislabel.
  if (mime == kPngMime) {
    SkBitmap bitmap = gfx::PNGCodec::Decode(bytes);
    return bitmap.isNull() ? gfx::JPEGCodec::Decode(bytes) : bitmap;
  }
  SkBitmap bitmap = gfx::JPEGCodec::Decode(bytes);
  return bitmap.isNull() ? gfx::PNGCodec::Decode(bytes) : bitmap;
}

// The `box` of the element whose `index` is `crop_index` in `elements`, when
// it carries a well-formed `[x, y, w, h]`.
std::optional<gfx::Rect> FindElementBox(const base::ListValue* elements,
                                        int crop_index) {
  if (!elements) {
    return std::nullopt;
  }
  for (const base::Value& value : *elements) {
    const base::DictValue* elem = value.GetIfDict();
    if (!elem || elem->FindInt("index") != crop_index) {
      continue;
    }
    const base::ListValue* box = elem->FindList("box");
    if (!box || box->size() != 4) {
      return std::nullopt;
    }
    for (const base::Value& v : *box) {
      if (!v.is_int()) {
        return std::nullopt;
      }
    }
    return gfx::Rect((*box)[0].GetInt(), (*box)[1].GetInt(),
                     (*box)[2].GetInt(), (*box)[3].GetInt());
  }
  return std::nullopt;
}

base::ListValue RectToList(const gfx::Rect& r) {
  base::ListValue list;
  list.Append(r.x());
  list.Append(r.y());
  list.Append(r.width());
  list.Append(r.height());
  return list;
}

// Shapes one tab's screenshot dict. `elements` is that tab's elements[] (a
// list, or null when the walk produced none / an unchanged stub).
void ShapeTabScreenshot(base::DictValue& shot,
                        const base::ListValue* elements,
                        std::optional<int> crop_index,
                        int max_edge) {
  const std::string* data_base64 = shot.FindString("data_base64");
  if (!data_base64 || data_base64->empty()) {
    return;
  }
  const std::string* mime_ptr = shot.FindString("mime");
  const std::string mime = mime_ptr ? *mime_ptr : std::string();

  std::optional<std::vector<uint8_t>> bytes = base::Base64Decode(*data_base64);
  if (!bytes) {
    shot.Set("shaping_error", "screenshot base64 could not be decoded");
    return;
  }
  SkBitmap bitmap = DecodeCapture(*bytes, mime);
  if (bitmap.isNull() || bitmap.width() <= 0 || bitmap.height() <= 0) {
    shot.Set("shaping_error",
             "screenshot image could not be decoded; delivered as captured");
    return;
  }
  const gfx::Size captured(bitmap.width(), bitmap.height());

  // ── Crop ────────────────────────────────────────────────────────────────
  std::optional<gfx::Rect> crop;
  if (crop_index.has_value()) {
    std::optional<gfx::Rect> box = FindElementBox(elements, *crop_index);
    if (box) {
      crop = ComputeScreenshotCropRect(*box, captured,
                                       kScreenshotCropPaddingPx);
    }
    if (!crop) {
      shot.Set("crop_error",
               base::StrCat(
                   {"Element ", base::NumberToString(*crop_index),
                    " has no visible box in this observation (it is not in "
                    "the current viewport, or is not an index from "
                    "elements[]). The FULL viewport was returned instead. "
                    "Bring it into view with BrowserScrollToElement, then "
                    "retry."}));
    }
  }

  SkBitmap working = bitmap;
  if (crop) {
    // Copy the subset into its own allocation rather than keep a view with a
    // non-zero pixel-ref origin: the resizer and the PNG encoder then see a
    // plain tightly-packed bitmap.
    SkBitmap subset;
    SkBitmap copied;
    if (!bitmap.extractSubset(&subset,
                              SkIRect::MakeXYWH(crop->x(), crop->y(),
                                                crop->width(),
                                                crop->height())) ||
        !copied.tryAllocPixels(subset.info()) ||
        !subset.readPixels(copied.pixmap())) {
      shot.Set("shaping_error", "screenshot crop failed; delivered as captured");
      return;
    }
    working = std::move(copied);
  }

  // ── Downscale ───────────────────────────────────────────────────────────
  const gfx::Size source(working.width(), working.height());
  const gfx::Size target = FitWithinMaxEdge(source, max_edge);
  const bool resized = target != source;
  if (resized) {
    working = skia::ImageOperations::Resize(
        working, skia::ImageOperations::RESIZE_BEST, target.width(),
        target.height());
    if (working.isNull()) {
      shot.Set("shaping_error",
               "screenshot downscale failed; delivered as captured");
      return;
    }
  }

  // ── Encode ──────────────────────────────────────────────────────────────
  // Already a PNG of exactly the delivered pixels → ship the capture's own
  // bytes; re-encoding would only spend CPU.
  if (crop || resized || mime != kPngMime) {
    std::optional<std::vector<uint8_t>> png =
        gfx::PNGCodec::EncodeBGRASkBitmap(working,
                                          /*discard_transparency=*/true);
    if (!png) {
      shot.Set("shaping_error",
               "screenshot PNG encode failed; delivered as captured");
      return;
    }
    shot.Set("mime", kPngMime);
    shot.Set("data_base64", base::Base64Encode(*png));
  }

  shot.Set("width", working.width());
  shot.Set("height", working.height());
  if (resized) {
    // Taken from the long edge, the one FitWithinMaxEdge sets exactly (the
    // short edge carries its rounding).
    const double scale =
        source.width() >= source.height()
            ? static_cast<double>(target.width()) / source.width()
            : static_cast<double>(target.height()) / source.height();
    shot.Set("scale", std::round(scale * 10000.0) / 10000.0);
  }
  if (crop) {
    base::DictValue crop_dict;
    crop_dict.Set("index", *crop_index);
    crop_dict.Set("box", RectToList(*crop));
    shot.Set("crop", std::move(crop_dict));
  }
}

}  // namespace

gfx::Size FitWithinMaxEdge(const gfx::Size& size, int max_edge) {
  const int long_edge = std::max(size.width(), size.height());
  if (max_edge <= 0 || long_edge <= max_edge) {
    return size;
  }
  const double factor = static_cast<double>(max_edge) / long_edge;
  const int w = std::max(1, static_cast<int>(std::lround(size.width() * factor)));
  const int h =
      std::max(1, static_cast<int>(std::lround(size.height() * factor)));
  return gfx::Size(std::min(w, max_edge), std::min(h, max_edge));
}

std::optional<gfx::Rect> ComputeScreenshotCropRect(const gfx::Rect& element_box,
                                                   const gfx::Size& image_size,
                                                   int padding) {
  if (element_box.IsEmpty()) {
    return std::nullopt;
  }
  gfx::Rect padded = element_box;
  padded.Outset(std::max(0, padding));
  padded.Intersect(gfx::Rect(image_size));
  if (padded.IsEmpty()) {
    return std::nullopt;
  }
  return padded;
}

base::DictValue ShapeScreenshotResult(base::DictValue result,
                                      std::optional<int> crop_index,
                                      int max_edge) {
  base::DictValue* observation = result.FindDict("observation");
  base::ListValue* tabs =
      observation ? observation->FindList("tabs") : nullptr;
  if (!tabs) {
    return result;
  }
  for (base::Value& tab_value : *tabs) {
    base::DictValue* tab = tab_value.GetIfDict();
    base::DictValue* shot = tab ? tab->FindDict("screenshot") : nullptr;
    if (!shot) {
      continue;
    }
    ShapeTabScreenshot(*shot, tab->FindList("elements"), crop_index, max_edge);
  }
  return result;
}

}  // namespace nexus::browser_tool_internal
