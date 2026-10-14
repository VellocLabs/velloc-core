// Copyright 2026 VellocLabs
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/utility/nexus/media/pdf_processor.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "custom_browser/utility/nexus/media/media_limits.h"
#include "pdf/pdf.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/gfx/codec/jpeg_codec.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/geometry/size_f.h"

namespace nexus {
namespace {

std::optional<std::pair<int, int>> ParsePdfPageRange(const std::string& pages) {
  std::string trimmed;
  base::TrimWhitespaceASCII(pages, base::TRIM_ALL, &trimmed);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  int first = 0;
  int last = 0;
  if (trimmed.ends_with("-")) {
    if (!base::StringToInt(trimmed.substr(0, trimmed.size() - 1), &first) ||
        first < 1) {
      return std::nullopt;
    }
    last = first + media_limits::kPdfMaxPagesPerRead - 1;
  } else {
    size_t dash = trimmed.find('-');
    if (dash == std::string::npos) {
      if (!base::StringToInt(trimmed, &first) || first < 1) {
        return std::nullopt;
      }
      last = first;
    } else if (!base::StringToInt(trimmed.substr(0, dash), &first) ||
               !base::StringToInt(trimmed.substr(dash + 1), &last) ||
               first < 1 || last < first) {
      return std::nullopt;
    }
  }
  if (last - first + 1 > media_limits::kPdfMaxPagesPerRead) {
    return std::nullopt;
  }
  return std::make_pair(first, last);
}

std::optional<std::vector<uint8_t>> EncodePdfPageAsJpeg(
    base::span<const uint8_t> pdf_bytes,
    int page_index) {
  std::optional<gfx::SizeF> page_size =
      chrome_pdf::GetPDFPageSizeByIndex(pdf_bytes, page_index);
  if (!page_size.has_value() || page_size->width() <= 0 ||
      page_size->height() <= 0) {
    return std::nullopt;
  }
  double scale = static_cast<double>(media_limits::kPdfRenderDpi) / 72.0;
  int width = std::max(1, static_cast<int>(std::ceil(page_size->width() * scale)));
  int height = std::max(1, static_cast<int>(std::ceil(page_size->height() * scale)));
  const int max_dimension = std::max(width, height);
  if (max_dimension > media_limits::kPdfRenderMaxDimension) {
    const double shrink = static_cast<double>(media_limits::kPdfRenderMaxDimension) / max_dimension;
    width = std::max(1, static_cast<int>(std::floor(width * shrink)));
    height = std::max(1, static_cast<int>(std::floor(height * shrink)));
  }
  SkBitmap bitmap;
  if (!bitmap.tryAllocN32Pixels(width, height)) {
    return std::nullopt;
  }
  bitmap.eraseColor(SK_ColorWHITE);
  chrome_pdf::RenderOptions options;
  options.stretch_to_bounds = false;
  options.keep_aspect_ratio = true;
  options.autorotate = true;
  options.use_color = true;
  options.render_device_type = chrome_pdf::RenderDeviceType::kDisplay;
  if (!chrome_pdf::RenderPDFPageToBitmap(
          pdf_bytes, page_index, bitmap.getPixels(), gfx::Size(width, height),
          gfx::Size(media_limits::kPdfRenderDpi, media_limits::kPdfRenderDpi), options)) {
    return std::nullopt;
  }
  return gfx::JPEGCodec::Encode(bitmap, 85);
}

}  // namespace

MediaReadOutcome ReadPdfMedia(base::FilePath path,
                                std::optional<std::string> pages) {
  MediaReadOutcome out;
  base::File::Info info;
  if (base::GetFileInfo(path, &info) && info.size > 0 &&
      static_cast<uint64_t>(info.size) > media_limits::kMediaReadByteCap) {
    out.error_code = "file_too_large";
    out.error_message = "PDF file exceeds maximum allowed size for Read.";
    return out;
  }
  std::optional<std::vector<uint8_t>> bytes = base::ReadFileToBytes(path);
  if (!bytes.has_value()) {
    out.error_code = "read_failed";
    out.error_message = "failed to read PDF file";
    return out;
  }
  if (bytes->size() > media_limits::kMediaReadByteCap) {
    out.error_code = "file_too_large";
    out.error_message = "PDF file exceeds maximum allowed size for Read.";
    return out;
  }
  if (bytes->size() < 5 ||
      std::string_view(reinterpret_cast<const char*>(bytes->data()), 5) !=
          "%PDF-") {
    out.error_code = "invalid_pdf";
    out.error_message = "File is not a valid PDF (missing %PDF- header).";
    return out;
  }
  int page_count = 0;
  if (!chrome_pdf::GetPDFDocInfo(base::span(*bytes), &page_count, nullptr) ||
      page_count <= 0) {
    out.error_code = "invalid_pdf";
    out.error_message = "PDF file is corrupted or invalid.";
    return out;
  }

  auto result = mojom::MediaReadResult::New();
  result->page_count = static_cast<uint32_t>(page_count);
  if (!pages.has_value()) {
    if (page_count > media_limits::kPdfInlinePageLimit) {
      out.error_code = "pdf_too_many_pages";
      // The count is load-bearing: the guest's @-mention step reads it back
      // for its pdf_reference note (nexus_web src/utils/attachments.ts).
      out.error_message = base::StrCat(
          {"This PDF has ", base::NumberToString(page_count),
           " pages, which is too many to read at once. Use the pages "
           "parameter to read specific page ranges (maximum ",
           base::NumberToString(media_limits::kPdfMaxPagesPerRead),
           " pages per request)."});
      return out;
    }
    result->parts.push_back(MakeTextMediaPart(base::StrCat({"PDF file read: ", path.AsUTF8Unsafe(), " (", base::NumberToString(page_count), " page(s))"})));
    result->parts.push_back(MakeBinaryMediaPart("document", "application/pdf", std::move(*bytes)));
    out.ok = true;
    out.result = std::move(result);
    return out;
  }

  std::optional<std::pair<int, int>> range = ParsePdfPageRange(*pages);
  if (!range.has_value()) {
    out.error_code = "invalid_args";
    out.error_message = "Invalid pages parameter.";
    return out;
  }
  int first_page = range->first;
  int last_page = std::min(range->second, page_count);
  if (first_page > page_count) {
    out.error_code = "invalid_args";
    out.error_message = "Page range starts after the end of the PDF.";
    return out;
  }
  // Name the file and the real page count, as the whole-document branch does.
  // Without it a page-range result is bare `<page>` markers around images:
  // the model cannot tell which of several parallel reads an image came from
  // (OpenAI-compatible transports move every image to a later message), and
  // never learns how many pages the PDF actually has.
  result->parts.push_back(MakeTextMediaPart(base::StrCat(
      {"PDF file read: ", path.AsUTF8Unsafe(), " (pages ",
       base::NumberToString(first_page), "-", base::NumberToString(last_page),
       " of ", base::NumberToString(page_count), ")"})));
  for (int page = first_page; page <= last_page; ++page) {
    std::optional<std::vector<uint8_t>> jpeg =
        EncodePdfPageAsJpeg(base::span(*bytes), page - 1);
    if (!jpeg.has_value()) {
      out.error_code = "pdf_render_failed";
      out.error_message = "failed to render PDF page";
      return out;
    }
    result->parts.push_back(MakeTextMediaPart(base::StrCat({"\n<page number=\"", base::NumberToString(page), "\">"})));
    result->parts.push_back(MakeBinaryMediaPart("image", "image/jpeg", std::move(*jpeg), static_cast<uint32_t>(page)));
    result->parts.push_back(MakeTextMediaPart("</page>"));
  }
  out.ok = true;
  out.result = std::move(result);
  return out;
}

}  // namespace nexus
