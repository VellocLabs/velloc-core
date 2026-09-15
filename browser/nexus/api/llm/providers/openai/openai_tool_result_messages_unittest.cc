// Copyright 2026 The Custom Browser Authors. All rights reserved.

#include "custom_browser/browser/nexus/api/llm/providers/openai/openai_tool_result_messages.h"

#include <string>
#include <utility>
#include <vector>

#include "base/values.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

base::DictValue TextResultPart(const std::string& id, const std::string& text) {
  base::DictValue text_part;
  text_part.Set("type", "text");
  text_part.Set("text", text);
  base::ListValue result;
  result.Append(std::move(text_part));
  base::DictValue part;
  part.Set("type", "tool-result");
  part.Set("toolCallId", id);
  part.Set("result", std::move(result));
  return part;
}

base::DictValue ImageResultPart(const std::string& id,
                                const std::string& text,
                                const std::string& base64) {
  base::DictValue part = TextResultPart(id, text);
  base::DictValue image_part;
  image_part.Set("type", "image");
  image_part.Set("image", base64);
  image_part.Set("mimeType", "image/png");
  part.FindList("result")->Append(std::move(image_part));
  return part;
}

// Regression: a round's parallel results arrive as ONE tool message with two
// `tool-result` parts. Each must become its own wire tool message — the Ollama
// serializer used to overwrite one dict, so only the last result survived.
TEST(OpenAiToolResultMessagesTest, ChatEmitsOneToolMessagePerResultPart) {
  base::ListValue content;
  content.Append(TextResultPart("call_a", "result A"));
  content.Append(TextResultPart("call_b", "result B"));

  base::ListValue messages;
  AppendChatCompletionToolMessages(content, messages);

  ASSERT_EQ(messages.size(), 2u);
  const base::DictValue& a = messages[0].GetDict();
  EXPECT_EQ(*a.FindString("role"), "tool");
  EXPECT_EQ(*a.FindString("tool_call_id"), "call_a");
  EXPECT_EQ(*a.FindString("content"), "result A");
  const base::DictValue& b = messages[1].GetDict();
  EXPECT_EQ(*b.FindString("role"), "tool");
  EXPECT_EQ(*b.FindString("tool_call_id"), "call_b");
  EXPECT_EQ(*b.FindString("content"), "result B");
}

// Regression: an image result in a parallel batch must not put its `user`
// message between the tool messages (`tool, user, tool` is rejected — tool
// messages must directly follow the assistant tool_calls).
TEST(OpenAiToolResultMessagesTest, ChatDefersImageUserMessageUntilAfterBatch) {
  base::ListValue content;
  content.Append(ImageResultPart("call_a", "shot", "AAAA"));
  content.Append(TextResultPart("call_b", "result B"));

  base::ListValue messages;
  AppendChatCompletionToolMessages(content, messages);

  ASSERT_EQ(messages.size(), 3u);
  EXPECT_EQ(*messages[0].GetDict().FindString("role"), "tool");
  EXPECT_EQ(*messages[0].GetDict().FindString("tool_call_id"), "call_a");
  // The tool's own text, untouched: browser tools put a JSON envelope here.
  EXPECT_EQ(*messages[0].GetDict().FindString("content"), "shot");
  EXPECT_EQ(*messages[1].GetDict().FindString("role"), "tool");
  EXPECT_EQ(*messages[1].GetDict().FindString("tool_call_id"), "call_b");
  EXPECT_EQ(*messages[1].GetDict().FindString("content"), "result B");

  const base::DictValue& user = messages[2].GetDict();
  EXPECT_EQ(*user.FindString("role"), "user");
  const base::ListValue* parts = user.FindList("content");
  ASSERT_TRUE(parts);
  // note, `Tool call call_a:`, the caption "shot", the image.
  ASSERT_EQ(parts->size(), 4u);
  EXPECT_EQ(*(*parts)[0].GetDict().FindString("type"), "text");
  EXPECT_EQ(*(*parts)[1].GetDict().FindString("text"), "Tool call call_a:");
  EXPECT_EQ(*(*parts)[2].GetDict().FindString("text"), "shot");
  const base::DictValue& img = (*parts)[3].GetDict();
  EXPECT_EQ(*img.FindString("type"), "image_url");
  EXPECT_EQ(*img.FindStringByDottedPath("image_url.url"),
            "data:image/png;base64,AAAA");
}

// A single text result keeps its shape: one tool message, no user message; a
// string result passes through verbatim.
TEST(OpenAiToolResultMessagesTest, ChatStringResultPassesThrough) {
  base::DictValue part;
  part.Set("type", "tool-result");
  part.Set("toolCallId", "call_a");
  part.Set("result", "plain");
  base::ListValue content;
  content.Append(std::move(part));

  base::ListValue messages;
  AppendChatCompletionToolMessages(content, messages);

  ASSERT_EQ(messages.size(), 1u);
  EXPECT_EQ(*messages[0].GetDict().FindString("content"), "plain");
}

TEST(OpenAiToolResultMessagesTest, ResponsesEmitsOneOutputItemPerResultPart) {
  base::ListValue content;
  content.Append(TextResultPart("call_a", "result A"));
  content.Append(TextResultPart("call_b", "result B"));

  base::ListValue input;
  AppendResponsesToolOutputItems(content, input);

  ASSERT_EQ(input.size(), 2u);
  EXPECT_EQ(*input[0].GetDict().FindString("type"), "function_call_output");
  EXPECT_EQ(*input[0].GetDict().FindString("call_id"), "call_a");
  EXPECT_EQ(*input[0].GetDict().FindString("output"), "result A");
  EXPECT_EQ(*input[1].GetDict().FindString("call_id"), "call_b");
  EXPECT_EQ(*input[1].GetDict().FindString("output"), "result B");
}

TEST(OpenAiToolResultMessagesTest,
     ResponsesDefersImageUserMessageUntilAfterBatch) {
  base::ListValue content;
  content.Append(ImageResultPart("call_a", "shot", "https://x/a.png"));
  content.Append(TextResultPart("call_b", "result B"));

  base::ListValue input;
  AppendResponsesToolOutputItems(content, input);

  ASSERT_EQ(input.size(), 3u);
  EXPECT_EQ(*input[0].GetDict().FindString("type"), "function_call_output");
  EXPECT_EQ(*input[0].GetDict().FindString("call_id"), "call_a");
  EXPECT_EQ(*input[1].GetDict().FindString("type"), "function_call_output");
  EXPECT_EQ(*input[1].GetDict().FindString("call_id"), "call_b");

  const base::DictValue& user = input[2].GetDict();
  EXPECT_EQ(*user.FindString("type"), "message");
  EXPECT_EQ(*user.FindString("role"), "user");
  const base::ListValue* parts = user.FindList("content");
  ASSERT_TRUE(parts);
  ASSERT_EQ(parts->size(), 4u);
  EXPECT_EQ(*(*parts)[1].GetDict().FindString("type"), "input_text");
  EXPECT_EQ(*(*parts)[1].GetDict().FindString("text"), "Tool call call_a:");
  EXPECT_EQ(*(*parts)[3].GetDict().FindString("type"), "input_image");
  EXPECT_EQ(*(*parts)[3].GetDict().FindString("image_url"), "https://x/a.png");
}

// One PDF page-range read as the media service returns it: a header, then
// `<page>` markers around each page image.
base::DictValue PdfPagesResultPart(const std::string& id,
                                   const std::string& path,
                                   const std::string& page_image) {
  base::ListValue result;
  auto add_text = [&result](const std::string& text) {
    base::DictValue rp;
    rp.Set("type", "text");
    rp.Set("text", text);
    result.Append(std::move(rp));
  };
  add_text("PDF file read: " + path + " (pages 1-1 of 1)");
  add_text("\n<page number=\"1\">");
  base::DictValue image;
  image.Set("type", "image");
  image.Set("image", page_image);
  image.Set("mimeType", "image/jpeg");
  result.Append(std::move(image));
  add_text("</page>");
  base::DictValue part;
  part.Set("type", "tool-result");
  part.Set("toolCallId", id);
  part.Set("result", std::move(result));
  return part;
}

// Regression: three PDFs read in parallel used to reach the model as three
// image-free tool messages plus one user message holding three unlabelled
// images, and the model attributed each page to the wrong file. Every image
// must now follow its tool call label and its file / page captions.
TEST(OpenAiToolResultMessagesTest, ChatLabelsHoistedImagesPerToolCall) {
  base::ListValue content;
  content.Append(PdfPagesResultPart("call_a", "a.pdf", "AAAA"));
  content.Append(PdfPagesResultPart("call_b", "b.pdf", "BBBB"));
  content.Append(PdfPagesResultPart("call_c", "c.pdf", "CCCC"));

  base::ListValue messages;
  AppendChatCompletionToolMessages(content, messages);

  ASSERT_EQ(messages.size(), 4u);
  const std::string* tool_b = messages[1].GetDict().FindString("content");
  ASSERT_TRUE(tool_b);
  EXPECT_EQ(*tool_b, "PDF file read: b.pdf (pages 1-1 of 1)\n\n"
                     "<page number=\"1\">\n</page>");

  const base::ListValue* parts = messages[3].GetDict().FindList("content");
  ASSERT_TRUE(parts);
  // Flatten to `text` / `<img:URL>` so the order is checked in one place.
  std::vector<std::string> seq;
  for (const auto& p : *parts) {
    const base::DictValue& d = p.GetDict();
    if (*d.FindString("type") == "image_url") {
      seq.push_back("<img:" + *d.FindStringByDottedPath("image_url.url") +
                    ">");
    } else {
      seq.push_back(*d.FindString("text"));
    }
  }
  const std::vector<std::string> expected = {
      seq[0],
      "Tool call call_a:",
      "PDF file read: a.pdf (pages 1-1 of 1)",
      "\n<page number=\"1\">",
      "<img:data:image/jpeg;base64,AAAA>",
      "</page>",
      "Tool call call_b:",
      "PDF file read: b.pdf (pages 1-1 of 1)",
      "\n<page number=\"1\">",
      "<img:data:image/jpeg;base64,BBBB>",
      "</page>",
      "Tool call call_c:",
      "PDF file read: c.pdf (pages 1-1 of 1)",
      "\n<page number=\"1\">",
      "<img:data:image/jpeg;base64,CCCC>",
      "</page>",
  };
  EXPECT_EQ(seq, expected);
}

// Long text stays in the tool message only; it is not repeated as a caption.
TEST(OpenAiToolResultMessagesTest, ChatDoesNotRepeatLongTextAsCaption) {
  const std::string long_text(301, 'x');
  base::ListValue content;
  content.Append(ImageResultPart("call_a", long_text, "AAAA"));

  base::ListValue messages;
  AppendChatCompletionToolMessages(content, messages);

  ASSERT_EQ(messages.size(), 2u);
  const base::ListValue* parts = messages[1].GetDict().FindList("content");
  ASSERT_TRUE(parts);
  ASSERT_EQ(parts->size(), 3u);  // note, label, image
  EXPECT_EQ(*(*parts)[2].GetDict().FindString("type"), "image_url");
}

// This transport has no document part: the marker must tell the model it has
// not seen the file and how to see it, instead of a bare `[document: ...]`.
TEST(OpenAiToolResultMessagesTest, ChatDocumentMarkerPointsToPageReads) {
  base::DictValue doc;
  doc.Set("type", "document");
  doc.Set("mimeType", "application/pdf");
  base::DictValue part = TextResultPart("call_a", "PDF file read: a.pdf");
  part.FindList("result")->Append(std::move(doc));
  base::ListValue content;
  content.Append(std::move(part));

  base::ListValue messages;
  AppendChatCompletionToolMessages(content, messages);

  ASSERT_EQ(messages.size(), 1u);
  const std::string* text = messages[0].GetDict().FindString("content");
  ASSERT_TRUE(text);
  EXPECT_EQ(*text, "PDF file read: a.pdf\n" +
                       DocumentUnavailableMarker(nullptr));
}

TEST(OpenAiToolResultMessagesTest, DocumentMarkerNamesMimeAndPageRead) {
  const std::string mime = "application/pdf";
  const std::string marker = DocumentUnavailableMarker(&mime);
  EXPECT_TRUE(marker.starts_with("[document: application/pdf - "));
  EXPECT_NE(marker.find("cannot be shown through this provider"),
            std::string::npos);
  EXPECT_NE(marker.find("pages parameter"), std::string::npos);
}

}  // namespace
}  // namespace nexus
