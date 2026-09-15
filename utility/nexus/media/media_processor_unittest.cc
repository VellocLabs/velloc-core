// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/utility/nexus/media/image_processor.h"
#include "custom_browser/utility/nexus/media/notebook_processor.h"
#include "custom_browser/utility/nexus/media/pdf_processor.h"

#include <optional>
#include <string>
#include <vector>

#include "base/base64.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/strings/string_number_conversions.h"
#include "base/test/task_environment.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "tools/v8_context_snapshot/buildflags.h"
#include "ui/gfx/codec/png_codec.h"
#include "testing/gtest/include/gtest/gtest.h"

#if defined(V8_USE_EXTERNAL_STARTUP_DATA)
#include "gin/v8_initializer.h"
#endif

namespace nexus {
namespace {

TEST(NexusMediaProcessorTest, ReadImageReturnsImagePart) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("pixel.png");
  constexpr uint8_t kPngBytes[] = {
      0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
      0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
      0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00,
      0x0D, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0xF8, 0xCF, 0xC0, 0x00,
      0x00, 0x03, 0x01, 0x01, 0x00, 0x18, 0xDD, 0x8D, 0xB0, 0x00, 0x00, 0x00,
      0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
  ASSERT_TRUE(base::WriteFile(file, base::span(kPngBytes)));

  MediaReadOutcome outcome = ReadImageMedia(file, "image/png");

  ASSERT_TRUE(outcome.ok) << outcome.error_message.value_or("");
  ASSERT_TRUE(outcome.result);
  ASSERT_GE(outcome.result->parts.size(), 2u);
  EXPECT_EQ(outcome.result->parts[1]->type, "image");
  EXPECT_EQ(outcome.result->parts[1]->mime_type, std::optional<std::string>("image/png"));
}

TEST(NexusMediaProcessorTest, ReadLargePngCompressesToJpeg) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("large.png");
  SkBitmap bitmap;
  ASSERT_TRUE(bitmap.tryAllocN32Pixels(2200, 1800));
  uint32_t seed = 0x12345678u;
  for (int y = 0; y < bitmap.height(); ++y) {
    for (int x = 0; x < bitmap.width(); ++x) {
      seed = seed * 1664525u + 1013904223u;
      *bitmap.getAddr32(x, y) = SkColorSetARGB(
          255, seed & 0xff, (seed >> 8) & 0xff, (seed >> 16) & 0xff);
    }
  }
  std::optional<std::vector<uint8_t>> png =
      gfx::PNGCodec::EncodeBGRASkBitmap(bitmap, false);
  ASSERT_TRUE(png.has_value());
  ASSERT_TRUE(base::WriteFile(file, base::span(*png)));

  MediaReadOutcome outcome = ReadImageMedia(file, "image/png");

  ASSERT_TRUE(outcome.ok) << outcome.error_message.value_or("");
  ASSERT_TRUE(outcome.result);
  ASSERT_GE(outcome.result->parts.size(), 2u);
  EXPECT_EQ(outcome.result->parts[1]->type, "image");
  EXPECT_EQ(outcome.result->parts[1]->mime_type, std::optional<std::string>("image/jpeg"));
}

TEST(NexusMediaProcessorTest, ReadGifAndWebpPassThroughWithoutCompression) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath gif = temp_dir.GetPath().AppendASCII("tiny.gif");
  const base::FilePath webp = temp_dir.GetPath().AppendASCII("tiny.webp");
  ASSERT_TRUE(base::WriteFile(gif, "GIF89a"));
  ASSERT_TRUE(base::WriteFile(webp, "RIFFxxxxWEBP"));

  MediaReadOutcome gif_out = ReadImageMedia(gif, "image/gif");
  MediaReadOutcome webp_out = ReadImageMedia(webp, "image/webp");

  ASSERT_TRUE(gif_out.ok) << gif_out.error_message.value_or("");
  ASSERT_TRUE(webp_out.ok) << webp_out.error_message.value_or("");
  ASSERT_GE(gif_out.result->parts.size(), 2u);
  ASSERT_GE(webp_out.result->parts.size(), 2u);
  EXPECT_EQ(gif_out.result->parts[1]->mime_type,
            std::optional<std::string>("image/gif"));
  EXPECT_EQ(webp_out.result->parts[1]->mime_type,
            std::optional<std::string>("image/webp"));
}
// pdfium initializes V8, which the utility process has loaded a snapshot
// for; this test binary has not (same as pdf/test/run_all_unittests.cc).
void LoadV8SnapshotForPdfium() {
#if defined(V8_USE_EXTERNAL_STARTUP_DATA)
#if BUILDFLAG(USE_V8_CONTEXT_SNAPSHOT)
  gin::V8Initializer::LoadV8Snapshot(
      gin::V8SnapshotFileType::kWithAdditionalContext);
#else
  gin::V8Initializer::LoadV8Snapshot(gin::V8SnapshotFileType::kDefault);
#endif
#endif
}

// A minimal valid PDF with `pages` blank US-letter pages and a correct xref.
std::string MakeBlankPdf(int pages) {
  std::vector<std::string> objects;
  std::string kids;
  for (int i = 0; i < pages; ++i) {
    kids += base::NumberToString(3 + i) + " 0 R ";
  }
  objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
  objects.push_back("<< /Type /Pages /Kids [" + kids + "] /Count " +
                    base::NumberToString(pages) + " >>");
  for (int i = 0; i < pages; ++i) {
    objects.push_back(
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>");
  }
  std::string pdf = "%PDF-1.4\n";
  std::vector<size_t> offsets;
  for (size_t i = 0; i < objects.size(); ++i) {
    offsets.push_back(pdf.size());
    pdf += base::NumberToString(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
  }
  const size_t xref = pdf.size();
  pdf += "xref\n0 " + base::NumberToString(objects.size() + 1) +
         "\n0000000000 65535 f \n";
  for (size_t offset : offsets) {
    std::string digits = base::NumberToString(offset);
    pdf += std::string(10 - digits.size(), '0') + digits + " 00000 n \n";
  }
  pdf += "trailer\n<< /Size " + base::NumberToString(objects.size() + 1) +
         " /Root 1 0 R >>\nstartxref\n" + base::NumberToString(xref) +
         "\n%%EOF\n";
  return pdf;
}

// Regression: a page-range read used to return only `<page>` markers around
// images. With several PDFs read in parallel the model could not tell which
// file an image came from, and it never learned the real page count (the
// guest's @-mention estimate claimed 13-15 pages for 2-page PDFs).
TEST(NexusMediaProcessorTest, ReadPdfPageRangeNamesFileAndRealPageCount) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  LoadV8SnapshotForPdfium();
  const base::FilePath file = temp_dir.GetPath().AppendASCII("claim.pdf");
  ASSERT_TRUE(base::WriteFile(file, MakeBlankPdf(2)));

  MediaReadOutcome outcome = ReadPdfMedia(file, "1-14");

  ASSERT_TRUE(outcome.ok) << outcome.error_message.value_or("");
  ASSERT_TRUE(outcome.result);
  EXPECT_EQ(outcome.result->page_count, 2u);
  ASSERT_FALSE(outcome.result->parts.empty());
  const auto& header = outcome.result->parts[0];
  ASSERT_TRUE(header);
  EXPECT_EQ(header->type, "text");
  EXPECT_EQ(header->text.value_or(""),
            "PDF file read: " + file.AsUTF8Unsafe() + " (pages 1-2 of 2)");
  int images = 0;
  for (const auto& part : outcome.result->parts) {
    images += part && part->type == "image" ? 1 : 0;
  }
  EXPECT_EQ(images, 2);
}

// A whole-document read of a PDF over the inline limit fails, and the error
// carries the real page count: the guest's @-mention step reads it back for
// its pdf_reference note instead of guessing from the file size.
TEST(NexusMediaProcessorTest, ReadPdfTooManyPagesReportsRealPageCount) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  LoadV8SnapshotForPdfium();
  const base::FilePath file = temp_dir.GetPath().AppendASCII("book.pdf");
  ASSERT_TRUE(base::WriteFile(file, MakeBlankPdf(12)));

  MediaReadOutcome outcome = ReadPdfMedia(file, std::nullopt);

  ASSERT_FALSE(outcome.ok);
  EXPECT_EQ(outcome.error_code.value_or(""), "pdf_too_many_pages");
  EXPECT_EQ(outcome.error_message.value_or(""),
            "This PDF has 12 pages, which is too many to read at once. Use "
            "the pages parameter to read specific page ranges (maximum 20 "
            "pages per request).");
}

TEST(NexusMediaProcessorTest, ReadNotebookReturnsTextAndEmbeddedImageParts) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("notebook.ipynb");
  ASSERT_TRUE(base::WriteFile(
      file,
      R"({"metadata":{"language_info":{"name":"python"}},"cells":[{"cell_type":"markdown","id":"intro","source":["# Title\n"]},{"cell_type":"code","id":"plot","source":"print('x')\n","outputs":[{"output_type":"display_data","data":{"text/plain":"<Figure>","image/png":"aGVsbG8=\n"}}]}]})"));

  MediaReadOutcome outcome = ReadNotebookMedia(file);

  ASSERT_TRUE(outcome.ok) << outcome.error_message.value_or("");
  ASSERT_TRUE(outcome.result);
  bool saw_text = false;
  bool saw_image = false;
  for (const auto& part : outcome.result->parts) {
    if (!part) continue;
    saw_text |= part->type == "text" && part->text.has_value() &&
                part->text->find("# Title") != std::string::npos;
    saw_image |= part->type == "image" && part->mime_type == "image/png";
  }
  EXPECT_TRUE(saw_text);
  EXPECT_TRUE(saw_image);
  ASSERT_TRUE(outcome.result->full_text_for_cache.has_value());
  EXPECT_FALSE(outcome.result->full_text_for_cache->empty());
}

}  // namespace
}  // namespace nexus
