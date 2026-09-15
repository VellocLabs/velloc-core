// Copyright 2026 The Custom Browser Authors. All rights reserved.

#include "custom_browser/browser/nexus/api/llm/providers/openai/openai_tool_result_messages.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/json/json_writer.h"

namespace nexus {

namespace {

constexpr char kImageNote[] =
    "The images below were returned by the tool calls above, grouped by tool "
    "call id.";

// Text parts at most this long are repeated next to the images they surround
// (`PDF file read: ...`, `<page number="2">`), so every image keeps its
// caption after being moved out of its tool message. Longer text stays only
// in the tool message.
constexpr size_t kMaxImageCaptionChars = 300;

// One part of the follow-up user message: a caption or an image URL.
struct HoistedPart {
  bool is_image = false;
  std::string value;
};

// The images of one tool result, with their captions, in result order.
struct ImageGroup {
  std::optional<std::string> tool_call_id;
  std::vector<HoistedPart> parts;
};

struct FlattenedToolResult {
  std::optional<std::string> tool_call_id;
  std::string text;
  // Captions and images in result order; empty unless the result has images.
  std::vector<HoistedPart> hoisted;
};

std::string ToolCallLabel(const std::optional<std::string>& tool_call_id) {
  return tool_call_id ? "Tool call " + *tool_call_id : "Tool call";
}

// Flattens one `tool-result` part: text parts joined by '\n', documents as a
// `[document: <mime>]` marker, images collected as URLs / data URIs. A string
// result passes through verbatim; any other value is JSON-encoded.
//
// Images cannot travel in a tool message, so they move to one user message
// after the batch. Moved bare, several parallel results (three PDFs read at
// once) arrive as an unlabelled run of images and the model pairs them with
// the wrong files. So each image keeps the short text parts around it, under
// its tool call id. The tool message text itself is left exactly as the tool
// produced it: browser tools return a JSON envelope there, and anything
// appended would stop it parsing.
FlattenedToolResult FlattenToolResult(const base::DictValue& part) {
  FlattenedToolResult out;
  if (const std::string* id = part.FindString("toolCallId")) {
    out.tool_call_id = *id;
  }
  bool has_image = false;
  if (const base::ListValue* result_parts = part.FindList("result")) {
    for (const auto& rp_val : *result_parts) {
      const base::DictValue* rp = rp_val.GetIfDict();
      if (!rp) {
        continue;
      }
      const std::string* rp_type = rp->FindString("type");
      if (rp_type && *rp_type == "text") {
        if (const std::string* text = rp->FindString("text")) {
          if (!out.text.empty()) {
            out.text += '\n';
          }
          out.text += *text;
          if (text->size() <= kMaxImageCaptionChars) {
            out.hoisted.push_back({/*is_image=*/false, *text});
          }
        }
      } else if (rp_type && *rp_type == "document") {
        const std::string* mime = rp->FindString("mimeType");
        if (!out.text.empty()) {
          out.text += '\n';
        }
        out.text += DocumentUnavailableMarker(mime);
      } else if (rp_type && *rp_type == "image") {
        const std::string* img = rp->FindString("image");
        if (img && !img->empty()) {
          has_image = true;
          if (img->starts_with("http://") || img->starts_with("https://") ||
              img->starts_with("data:")) {
            out.hoisted.push_back({/*is_image=*/true, *img});
          } else {
            const std::string* mime = rp->FindString("mimeType");
            out.hoisted.push_back(
                {/*is_image=*/true,
                 "data:" + (mime ? *mime : std::string("image/jpeg")) +
                     ";base64," + *img});
          }
        }
      }
    }
  } else if (const std::string* result_str = part.FindString("result")) {
    out.text = *result_str;
  } else if (const base::Value* result_val = part.Find("result")) {
    base::JSONWriter::Write(*result_val, &out.text);
  }
  if (!has_image) {
    out.hoisted.clear();
  }
  return out;
}

// Calls `emit` for every `tool-result` part and returns the image groups of
// the whole batch, in part order.
template <typename Emit>
std::vector<ImageGroup> ForEachToolResult(const base::ListValue& content_parts,
                                          Emit emit) {
  std::vector<ImageGroup> groups;
  for (const auto& part_val : content_parts) {
    const base::DictValue* part = part_val.GetIfDict();
    if (!part) {
      continue;
    }
    const std::string* type = part->FindString("type");
    if (!type || *type != "tool-result") {
      continue;
    }
    FlattenedToolResult flat = FlattenToolResult(*part);
    if (!flat.hoisted.empty()) {
      groups.push_back({flat.tool_call_id, std::move(flat.hoisted)});
    }
    emit(flat);
  }
  return groups;
}

// Builds the follow-up user message's content: the note, then per group a
// `Tool call <id>:` label followed by its captions and images in order.
template <typename MakeText, typename MakeImage>
base::ListValue BuildImageMessageParts(std::vector<ImageGroup>& groups,
                                       MakeText make_text,
                                       MakeImage make_image) {
  base::ListValue parts;
  parts.Append(make_text(kImageNote));
  for (ImageGroup& group : groups) {
    parts.Append(make_text(ToolCallLabel(group.tool_call_id) + ":"));
    for (HoistedPart& hp : group.parts) {
      parts.Append(hp.is_image ? make_image(std::move(hp.value))
                               : make_text(std::move(hp.value)));
    }
  }
  return parts;
}

}  // namespace

std::string DocumentUnavailableMarker(const std::string* mime_type) {
  return "[document: " +
         (mime_type ? *mime_type : std::string("application/pdf")) +
         " - its content cannot be shown through this provider. Read it with "
         "the pages parameter (e.g. pages: \"1-5\") to receive the pages as "
         "images.]";
}

void AppendChatCompletionToolMessages(const base::ListValue& content_parts,
                                      base::ListValue& messages) {
  std::vector<ImageGroup> groups = ForEachToolResult(
      content_parts, [&messages](FlattenedToolResult& flat) {
        base::DictValue tool_msg;
        tool_msg.Set("role", "tool");
        if (flat.tool_call_id) {
          tool_msg.Set("tool_call_id", *flat.tool_call_id);
        }
        tool_msg.Set("content", std::move(flat.text));
        messages.Append(std::move(tool_msg));
      });
  if (groups.empty()) {
    return;
  }

  base::ListValue parts = BuildImageMessageParts(
      groups,
      [](std::string text) {
        base::DictValue part;
        part.Set("type", "text");
        part.Set("text", std::move(text));
        return part;
      },
      [](std::string uri) {
        base::DictValue img_url;
        img_url.Set("url", std::move(uri));
        base::DictValue part;
        part.Set("type", "image_url");
        part.Set("image_url", std::move(img_url));
        return part;
      });
  base::DictValue user_msg;
  user_msg.Set("role", "user");
  user_msg.Set("content", std::move(parts));
  messages.Append(std::move(user_msg));
}

void AppendResponsesToolOutputItems(const base::ListValue& content_parts,
                                    base::ListValue& input) {
  std::vector<ImageGroup> groups = ForEachToolResult(
      content_parts, [&input](FlattenedToolResult& flat) {
        base::DictValue item;
        item.Set("type", "function_call_output");
        if (flat.tool_call_id) {
          item.Set("call_id", *flat.tool_call_id);
        }
        item.Set("output", std::move(flat.text));
        input.Append(std::move(item));
      });
  if (groups.empty()) {
    return;
  }

  base::ListValue parts = BuildImageMessageParts(
      groups,
      [](std::string text) {
        base::DictValue part;
        part.Set("type", "input_text");
        part.Set("text", std::move(text));
        return part;
      },
      [](std::string uri) {
        base::DictValue part;
        part.Set("type", "input_image");
        part.Set("image_url", std::move(uri));
        return part;
      });
  base::DictValue user_item;
  user_item.Set("type", "message");
  user_item.Set("role", "user");
  user_item.Set("content", std::move(parts));
  input.Append(std::move(user_item));
}

}  // namespace nexus
