// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/utility/nexus/media/image_processor.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string_view>
#include <utility>

#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "custom_browser/utility/nexus/media/media_limits.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkData.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "third_party/skia/include/core/SkPixmap.h"
#include "third_party/skia/include/core/SkSamplingOptions.h"
#include "third_party/skia/include/codec/SkCodec.h"
#include "ui/gfx/codec/jpeg_codec.h"
#include "ui/gfx/codec/png_codec.h"

namespace nexus {
namespace {

struct EncodedImage {
  std::vector<uint8_t> bytes;
  std::string mime_type;
};

std::optional<SkBitmap> DecodeImage(base::span<const uint8_t> bytes,
                                    std::string_view mime_type) {
  if (mime_type == "image/png") {
    SkBitmap bitmap = gfx::PNGCodec::Decode(bytes);
    if (!bitmap.drawsNothing()) return bitmap;
  }
  if (mime_type == "image/jpeg") {
    SkBitmap bitmap = gfx::JPEGCodec::Decode(bytes);
    if (!bitmap.drawsNothing()) return bitmap;
  }

  sk_sp<SkData> data = SkData::MakeWithCopy(bytes.data(), bytes.size());
  if (!data) return std::nullopt;
  std::unique_ptr<SkCodec> codec = SkCodec::MakeFromData(std::move(data));
  if (!codec) return std::nullopt;
  const SkImageInfo info = codec->getInfo().makeColorType(kN32_SkColorType).makeAlphaType(kPremul_SkAlphaType);
  SkBitmap bitmap;
  if (!bitmap.tryAllocPixels(info)) return std::nullopt;
  SkCodec::Result result = codec->getPixels(info, bitmap.getPixels(), bitmap.rowBytes());
  if (result != SkCodec::kSuccess && result != SkCodec::kIncompleteInput) {
    return std::nullopt;
  }
  return bitmap;
}

EncodedImage MaybeCompress(const std::vector<uint8_t>& bytes,
                           const std::string& mime_type) {
  if (bytes.size() <= media_limits::kImageCompressionThreshold) {
    return EncodedImage{bytes, mime_type};
  }
  std::optional<SkBitmap> decoded = DecodeImage(base::span(bytes), mime_type);
  if (!decoded.has_value()) return EncodedImage{bytes, mime_type};

  SkBitmap bitmap = std::move(*decoded);
  SkBitmap* encode_bitmap = &bitmap;
  SkBitmap scaled;
  const int max_dimension = std::max(bitmap.width(), bitmap.height());
  if (max_dimension > media_limits::kImageMaxDimension) {
    const double scale =
        static_cast<double>(media_limits::kImageMaxDimension) / max_dimension;
    const int width = std::max(1, static_cast<int>(std::floor(bitmap.width() * scale)));
    const int height = std::max(1, static_cast<int>(std::floor(bitmap.height() * scale)));
    if (scaled.tryAllocN32Pixels(width, height)) {
      SkPixmap dst;
      if (scaled.peekPixels(&dst) &&
          bitmap.pixmap().scalePixels(dst, SkSamplingOptions(SkFilterMode::kLinear))) {
        encode_bitmap = &scaled;
      }
    }
  }
  std::optional<std::vector<uint8_t>> jpeg =
      gfx::JPEGCodec::Encode(*encode_bitmap, 82);
  if (jpeg.has_value() && !jpeg->empty() && jpeg->size() < bytes.size()) {
    return EncodedImage{std::move(*jpeg), "image/jpeg"};
  }
  return EncodedImage{bytes, mime_type};
}

}  // namespace

MediaReadOutcome ReadImageMedia(const base::FilePath& path,
                                const std::string& mime_type) {
  base::File::Info info;
  if (base::GetFileInfo(path, &info) && info.size > 0 &&
      static_cast<uint64_t>(info.size) > media_limits::kMediaReadByteCap) {
    return MakeMediaError("file_too_large",
                          "image file exceeds maximum allowed size for Read");
  }
  std::optional<std::vector<uint8_t>> bytes = base::ReadFileToBytes(path);
  if (!bytes.has_value()) {
    return MakeMediaError("read_failed", "failed to read image file");
  }
  if (bytes->size() > media_limits::kMediaReadByteCap) {
    return MakeMediaError("file_too_large",
                          "image file exceeds maximum allowed size for Read");
  }

  EncodedImage encoded = MaybeCompress(*bytes, mime_type);
  auto result = mojom::MediaReadResult::New();
  result->parts.push_back(MakeTextMediaPart(base::StrCat(
      {"Image file read: ", path.AsUTF8Unsafe(), " (",
       base::NumberToString(bytes->size()), " bytes)"})));
  result->parts.push_back(MakeBinaryMediaPart("image", encoded.mime_type,
                                              std::move(encoded.bytes)));
  MediaReadOutcome outcome;
  outcome.ok = true;
  outcome.result = std::move(result);
  return outcome;
}

}  // namespace nexus
