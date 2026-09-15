// Copyright 2026 The Custom Browser Authors. All rights reserved.

#include "custom_browser/browser/nexus/api/llm/providers/openai/openai_user_content.h"

#include <string>
#include <utility>

#include "base/values.h"
#include "custom_browser/browser/nexus/api/llm/providers/openai/openai_tool_result_messages.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

base::DictValue TextPart(const std::string& text) {
  base::DictValue part;
  part.Set("type", "text");
  part.Set("text", text);
  return part;
}

// The shape nexus_llm_host.cc produces for a pasted image.
base::DictValue ImagePart(const std::string& base64,
                          const std::string& mime_type) {
  base::DictValue part;
  part.Set("type", "image");
  part.Set("image", base64);
  part.Set("mimeType", mime_type);
  return part;
}

// The shape nexus_llm_host.cc produces for an @-mentioned PDF.
base::DictValue DocumentPart(const std::string& base64) {
  base::DictValue part;
  part.Set("type", "document");
  part.Set("data", base64);
  part.Set("mimeType", "application/pdf");
  return part;
}

// A text-only message keeps the plain-string content earlier requests had.
TEST(OpenAiUserContentTest, TextOnlyStaysOneString) {
  base::ListValue parts;
  parts.Append(TextPart("first"));
  parts.Append(TextPart("second"));

  base::Value content =
      BuildResponsesUserContent(parts, /*accepts_input_file=*/true);

  ASSERT_TRUE(content.is_string());
  EXPECT_EQ(content.GetString(), "first\nsecond");
}

// Regression: the Responses path joined only text parts into a string, so a
// pasted screenshot never reached a Codex / GPT model.
TEST(OpenAiUserContentTest, ImageBecomesInputImage) {
  base::ListValue parts;
  parts.Append(TextPart("what is in this screenshot?"));
  parts.Append(ImagePart("iVBORw0KGgo=", "image/png"));

  base::Value content =
      BuildResponsesUserContent(parts, /*accepts_input_file=*/false);

  ASSERT_TRUE(content.is_list());
  const base::ListValue& list = content.GetList();
  ASSERT_EQ(list.size(), 2u);
  EXPECT_EQ(*list[0].GetDict().FindString("type"), "input_text");
  EXPECT_EQ(*list[0].GetDict().FindString("text"),
            "what is in this screenshot?");
  EXPECT_EQ(*list[1].GetDict().FindString("type"), "input_image");
  EXPECT_EQ(*list[1].GetDict().FindString("image_url"),
            "data:image/png;base64,iVBORw0KGgo=");
}

// An image that already carries a URL is passed through, not re-wrapped.
TEST(OpenAiUserContentTest, ImageUrlPassesThrough) {
  base::ListValue parts;
  parts.Append(ImagePart("data:image/jpeg;base64,/9j/", "image/jpeg"));

  base::Value content =
      BuildResponsesUserContent(parts, /*accepts_input_file=*/true);

  ASSERT_TRUE(content.is_list());
  EXPECT_EQ(*content.GetList()[0].GetDict().FindString("image_url"),
            "data:image/jpeg;base64,/9j/");
}

// The public /v1/responses API reads a PDF as an input_file with a data URI.
TEST(OpenAiUserContentTest, DocumentBecomesInputFile) {
  base::ListValue parts;
  parts.Append(TextPart("Called the Read tool"));
  parts.Append(DocumentPart("JVBERi0x"));

  base::Value content =
      BuildResponsesUserContent(parts, /*accepts_input_file=*/true);

  ASSERT_TRUE(content.is_list());
  const base::ListValue& list = content.GetList();
  ASSERT_EQ(list.size(), 2u);
  const base::DictValue& file = list[1].GetDict();
  EXPECT_EQ(*file.FindString("type"), "input_file");
  EXPECT_EQ(*file.FindString("filename"), "document.pdf");
  EXPECT_EQ(*file.FindString("file_data"),
            "data:application/pdf;base64,JVBERi0x");
}

// The Codex backend has no input_file: the document becomes the visible
// marker (never silently dropped), and images in the same message still go.
TEST(OpenAiUserContentTest, DocumentWithoutInputFileBecomesMarker) {
  base::ListValue parts;
  parts.Append(DocumentPart("JVBERi0x"));
  parts.Append(ImagePart("iVBORw0KGgo=", "image/png"));

  base::Value content =
      BuildResponsesUserContent(parts, /*accepts_input_file=*/false);

  ASSERT_TRUE(content.is_list());
  const base::ListValue& list = content.GetList();
  ASSERT_EQ(list.size(), 2u);
  const std::string mime = "application/pdf";
  EXPECT_EQ(*list[0].GetDict().FindString("type"), "input_text");
  EXPECT_EQ(*list[0].GetDict().FindString("text"),
            DocumentUnavailableMarker(&mime));
  EXPECT_EQ(*list[1].GetDict().FindString("type"), "input_image");
}

// ImageUrlForPart wraps raw base64 from `data` too, defaulting to image/png.
TEST(OpenAiUserContentTest, ImageUrlForPartWrapsRawData) {
  base::DictValue part;
  part.Set("type", "image");
  part.Set("data", "AAAA");

  EXPECT_EQ(ImageUrlForPart(part), "data:image/png;base64,AAAA");

  base::DictValue empty;
  empty.Set("type", "image");
  EXPECT_EQ(ImageUrlForPart(empty), std::nullopt);
}

}  // namespace
}  // namespace nexus
