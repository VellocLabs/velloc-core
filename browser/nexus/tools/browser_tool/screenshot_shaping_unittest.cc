// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/tools/browser_tool/screenshot_shaping.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/values.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkColor.h"
#include "third_party/skia/include/core/SkPaint.h"
#include "third_party/skia/include/core/SkRect.h"
#include "ui/gfx/codec/jpeg_codec.h"
#include "ui/gfx/codec/png_codec.h"

namespace nexus::browser_tool_internal {
namespace {

// A white `width`×`height` capture with an opaque red `red` rectangle.
SkBitmap MakeCapture(int width, int height, const SkIRect& red) {
  SkBitmap bitmap;
  bitmap.allocN32Pixels(width, height, /*isOpaque=*/true);
  bitmap.eraseColor(SK_ColorWHITE);
  SkCanvas canvas(bitmap);
  SkPaint paint;
  paint.setColor(SK_ColorRED);
  canvas.drawIRect(red, paint);
  return bitmap;
}

std::string PngBase64(const SkBitmap& bitmap) {
  std::optional<std::vector<uint8_t>> png =
      gfx::PNGCodec::EncodeBGRASkBitmap(bitmap, /*discard_transparency=*/true);
  EXPECT_TRUE(png.has_value());
  return base::Base64Encode(*png);
}

base::ListValue Box(int x, int y, int w, int h) {
  base::ListValue box;
  box.Append(x);
  box.Append(y);
  box.Append(w);
  box.Append(h);
  return box;
}

// `{action, observation:{tabs:[{tab_id, screenshot, elements}]}}` — the shape
// BuildObservationDict produces for BrowserScreenshot.
base::DictValue MakeResult(const std::string& mime,
                           const std::string& data_base64,
                           base::ListValue elements) {
  base::DictValue shot;
  shot.Set("mime", mime);
  shot.Set("data_base64", data_base64);
  base::DictValue tab;
  tab.Set("tab_id", 1);
  tab.Set("screenshot", std::move(shot));
  tab.Set("elements", std::move(elements));
  base::ListValue tabs;
  tabs.Append(std::move(tab));
  base::DictValue observation;
  observation.Set("tabs", std::move(tabs));
  base::DictValue result;
  result.Set("action", "screenshot");
  result.Set("observation", std::move(observation));
  return result;
}

base::ListValue OneElement(int index, base::ListValue box) {
  base::DictValue elem;
  elem.Set("index", index);
  elem.Set("type", "IMAGE");
  elem.Set("box", std::move(box));
  base::ListValue elements;
  elements.Append(std::move(elem));
  return elements;
}

const base::DictValue& ShotOf(const base::DictValue& result) {
  const base::ListValue* tabs =
      result.FindDict("observation")->FindList("tabs");
  return *(*tabs)[0].GetDict().FindDict("screenshot");
}

SkBitmap DecodeShot(const base::DictValue& shot) {
  std::optional<std::vector<uint8_t>> bytes =
      base::Base64Decode(*shot.FindString("data_base64"));
  EXPECT_TRUE(bytes.has_value());
  return gfx::PNGCodec::Decode(*bytes);
}

// ── FitWithinMaxEdge ───────────────────────────────────────────────────────

TEST(FitWithinMaxEdgeTest, FittingSizeIsUnchanged) {
  EXPECT_EQ(FitWithinMaxEdge(gfx::Size(1000, 600), 1024),
            gfx::Size(1000, 600));
  EXPECT_EQ(FitWithinMaxEdge(gfx::Size(1024, 1024), 1024),
            gfx::Size(1024, 1024));
}

TEST(FitWithinMaxEdgeTest, LandscapeScalesByLongEdge) {
  // A 1000×600 CSS viewport captured at 150% device scale.
  EXPECT_EQ(FitWithinMaxEdge(gfx::Size(1500, 900), 1024),
            gfx::Size(1024, 614));
}

TEST(FitWithinMaxEdgeTest, PortraitScalesByLongEdge) {
  EXPECT_EQ(FitWithinMaxEdge(gfx::Size(900, 3000), 1024),
            gfx::Size(307, 1024));
}

TEST(FitWithinMaxEdgeTest, ExtremeAspectKeepsOnePixel) {
  EXPECT_EQ(FitWithinMaxEdge(gfx::Size(10000, 2), 1024), gfx::Size(1024, 1));
}

TEST(FitWithinMaxEdgeTest, NonPositiveCapDisablesScaling) {
  EXPECT_EQ(FitWithinMaxEdge(gfx::Size(4000, 3000), 0), gfx::Size(4000, 3000));
}

// ── ComputeScreenshotCropRect ──────────────────────────────────────────────

TEST(ComputeScreenshotCropRectTest, PadsOnEverySide) {
  EXPECT_EQ(ComputeScreenshotCropRect(gfx::Rect(100, 80, 50, 40),
                                      gfx::Size(400, 300), 16),
            gfx::Rect(84, 64, 82, 72));
}

TEST(ComputeScreenshotCropRectTest, ClampsToTheImage) {
  // Near the top-left corner, and running off the right/bottom edges.
  EXPECT_EQ(ComputeScreenshotCropRect(gfx::Rect(5, 5, 20, 20),
                                      gfx::Size(400, 300), 16),
            gfx::Rect(0, 0, 41, 41));
  EXPECT_EQ(ComputeScreenshotCropRect(gfx::Rect(380, 290, 100, 100),
                                      gfx::Size(400, 300), 16),
            gfx::Rect(364, 274, 36, 26));
}

TEST(ComputeScreenshotCropRectTest, OutsideTheImageIsNullopt) {
  EXPECT_EQ(ComputeScreenshotCropRect(gfx::Rect(1000, 1000, 50, 50),
                                      gfx::Size(400, 300), 16),
            std::nullopt);
}

TEST(ComputeScreenshotCropRectTest, EmptyBoxIsNullopt) {
  EXPECT_EQ(ComputeScreenshotCropRect(gfx::Rect(10, 10, 0, 0),
                                      gfx::Size(400, 300), 16),
            std::nullopt);
}

// ── ShapeScreenshotResult ──────────────────────────────────────────────────

TEST(ShapeScreenshotResultTest, OversizedCaptureIsCappedAtTheLongEdge) {
  SkBitmap capture = MakeCapture(1500, 900, SkIRect::MakeXYWH(0, 0, 10, 10));
  base::DictValue shaped = ShapeScreenshotResult(
      MakeResult("image/png", PngBase64(capture), base::ListValue()),
      std::nullopt, kScreenshotMaxEdge);

  const base::DictValue& shot = ShotOf(shaped);
  EXPECT_EQ(*shot.FindString("mime"), "image/png");
  EXPECT_EQ(shot.FindInt("width"), 1024);
  EXPECT_EQ(shot.FindInt("height"), 614);
  ASSERT_TRUE(shot.FindDouble("scale").has_value());
  EXPECT_NEAR(*shot.FindDouble("scale"), 1024.0 / 1500.0, 1e-4);
  EXPECT_EQ(shot.Find("crop"), nullptr);
  EXPECT_EQ(shot.Find("shaping_error"), nullptr);

  SkBitmap delivered = DecodeShot(shot);
  ASSERT_FALSE(delivered.isNull());
  EXPECT_EQ(delivered.width(), 1024);
  EXPECT_EQ(delivered.height(), 614);
}

TEST(ShapeScreenshotResultTest, FittingPngShipsTheCapturedBytesUntouched) {
  SkBitmap capture = MakeCapture(1000, 600, SkIRect::MakeXYWH(0, 0, 10, 10));
  const std::string original = PngBase64(capture);
  base::DictValue shaped = ShapeScreenshotResult(
      MakeResult("image/png", original, base::ListValue()), std::nullopt,
      kScreenshotMaxEdge);

  const base::DictValue& shot = ShotOf(shaped);
  EXPECT_EQ(*shot.FindString("data_base64"), original);
  EXPECT_EQ(shot.FindInt("width"), 1000);
  EXPECT_EQ(shot.FindInt("height"), 600);
  EXPECT_EQ(shot.Find("scale"), nullptr);
}

TEST(ShapeScreenshotResultTest, JpegCaptureIsDeliveredAsPng) {
  // A lossy capture (upstream's default format) must not reach the model as
  // JPEG — text edges are exactly what JPEG smears.
  SkBitmap capture = MakeCapture(200, 100, SkIRect::MakeXYWH(0, 0, 10, 10));
  std::optional<std::vector<uint8_t>> jpeg =
      gfx::JPEGCodec::Encode(capture, /*quality=*/40);
  ASSERT_TRUE(jpeg.has_value());
  base::DictValue shaped = ShapeScreenshotResult(
      MakeResult("image/jpeg", base::Base64Encode(*jpeg), base::ListValue()),
      std::nullopt, kScreenshotMaxEdge);

  const base::DictValue& shot = ShotOf(shaped);
  EXPECT_EQ(*shot.FindString("mime"), "image/png");
  SkBitmap delivered = DecodeShot(shot);
  ASSERT_FALSE(delivered.isNull());
  EXPECT_EQ(delivered.width(), 200);
  EXPECT_EQ(delivered.height(), 100);
}

TEST(ShapeScreenshotResultTest, CropsToTheElementBoxWithPadding) {
  SkBitmap capture = MakeCapture(400, 300, SkIRect::MakeXYWH(100, 80, 50, 40));
  base::DictValue shaped = ShapeScreenshotResult(
      MakeResult("image/png", PngBase64(capture),
                 OneElement(7, Box(100, 80, 50, 40))),
      7, kScreenshotMaxEdge);

  const base::DictValue& shot = ShotOf(shaped);
  EXPECT_EQ(shot.Find("crop_error"), nullptr);
  EXPECT_EQ(shot.FindInt("width"), 82);
  EXPECT_EQ(shot.FindInt("height"), 72);
  const base::DictValue* crop = shot.FindDict("crop");
  ASSERT_NE(crop, nullptr);
  EXPECT_EQ(crop->FindInt("index"), 7);
  const base::ListValue* box = crop->FindList("box");
  ASSERT_NE(box, nullptr);
  EXPECT_EQ(*box, Box(84, 64, 82, 72));

  // The element sits inside the 16px padding: red in the middle, white at the
  // padded corner.
  SkBitmap delivered = DecodeShot(shot);
  ASSERT_FALSE(delivered.isNull());
  EXPECT_EQ(delivered.getColor(41, 36), SK_ColorRED);
  EXPECT_EQ(delivered.getColor(0, 0), SK_ColorWHITE);
  EXPECT_EQ(delivered.getColor(16, 16), SK_ColorRED);
  EXPECT_EQ(delivered.getColor(15, 15), SK_ColorWHITE);
}

TEST(ShapeScreenshotResultTest, LargeCropIsAlsoCapped) {
  SkBitmap capture =
      MakeCapture(3000, 2000, SkIRect::MakeXYWH(0, 0, 2800, 1000));
  base::DictValue shaped = ShapeScreenshotResult(
      MakeResult("image/png", PngBase64(capture),
                 OneElement(3, Box(16, 16, 2768, 968))),
      3, kScreenshotMaxEdge);

  const base::DictValue& shot = ShotOf(shaped);
  // Crop 2800×1000 → cap 1024×366.
  EXPECT_EQ(shot.FindInt("width"), 1024);
  EXPECT_EQ(shot.FindInt("height"), 366);
  ASSERT_NE(shot.FindDict("crop"), nullptr);
  EXPECT_EQ(*shot.FindDict("crop")->FindList("box"), Box(0, 0, 2800, 1000));
  EXPECT_TRUE(shot.FindDouble("scale").has_value());
}

TEST(ShapeScreenshotResultTest, UnknownIndexFallsBackLoudlyToFullViewport) {
  SkBitmap capture = MakeCapture(400, 300, SkIRect::MakeXYWH(100, 80, 50, 40));
  base::DictValue shaped = ShapeScreenshotResult(
      MakeResult("image/png", PngBase64(capture),
                 OneElement(7, Box(100, 80, 50, 40))),
      99, kScreenshotMaxEdge);

  const base::DictValue& shot = ShotOf(shaped);
  const std::string* crop_error = shot.FindString("crop_error");
  ASSERT_NE(crop_error, nullptr);
  EXPECT_NE(crop_error->find("99"), std::string::npos);
  EXPECT_NE(crop_error->find("FULL viewport"), std::string::npos);
  EXPECT_EQ(shot.Find("crop"), nullptr);
  EXPECT_EQ(shot.FindInt("width"), 400);
  EXPECT_EQ(shot.FindInt("height"), 300);
}

TEST(ShapeScreenshotResultTest, ElementWithoutBoxFallsBackLoudly) {
  base::DictValue elem;
  elem.Set("index", 7);
  elem.Set("type", "ANCHOR");
  base::ListValue elements;
  elements.Append(std::move(elem));
  SkBitmap capture = MakeCapture(400, 300, SkIRect::MakeXYWH(0, 0, 1, 1));
  base::DictValue shaped = ShapeScreenshotResult(
      MakeResult("image/png", PngBase64(capture), std::move(elements)), 7,
      kScreenshotMaxEdge);

  const base::DictValue& shot = ShotOf(shaped);
  EXPECT_NE(shot.FindString("crop_error"), nullptr);
  EXPECT_EQ(shot.FindInt("width"), 400);
}

TEST(ShapeScreenshotResultTest, UndecodableCapturePassesThroughWithError) {
  const std::string garbage = base::Base64Encode("not an image");
  base::DictValue shaped = ShapeScreenshotResult(
      MakeResult("image/png", garbage, base::ListValue()), std::nullopt,
      kScreenshotMaxEdge);

  const base::DictValue& shot = ShotOf(shaped);
  EXPECT_NE(shot.FindString("shaping_error"), nullptr);
  EXPECT_EQ(*shot.FindString("data_base64"), garbage);
  EXPECT_EQ(shot.Find("width"), nullptr);
}

TEST(ShapeScreenshotResultTest, ResultWithoutScreenshotIsUnchanged) {
  base::DictValue tab;
  tab.Set("tab_id", 1);
  base::ListValue tabs;
  tabs.Append(std::move(tab));
  base::DictValue observation;
  observation.Set("tabs", std::move(tabs));
  base::DictValue result;
  result.Set("action", "screenshot");
  result.Set("observation", std::move(observation));
  const base::DictValue expected = result.Clone();

  EXPECT_EQ(ShapeScreenshotResult(std::move(result), 4, kScreenshotMaxEdge),
            expected);
}

}  // namespace
}  // namespace nexus::browser_tool_internal
