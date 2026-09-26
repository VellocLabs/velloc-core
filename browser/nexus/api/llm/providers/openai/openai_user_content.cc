// Copyright 2026 The Custom Browser Authors. All rights reserved.

#include "custom_browser/browser/nexus/api/llm/providers/openai/openai_user_content.h"

#include <string>
#include <utility>

#include "base/files/file_path.h"
#include "custom_browser/browser/nexus/api/llm/providers/openai/openai_tool_result_messages.h"
#include "net/base/mime_util.h"

namespace nexus {

namespace {

bool IsUrlOrDataUri(const std::string& value) {
  return value.starts_with("http://") || value.starts_with("https://") ||
         value.starts_with("data:");
}

std::string DataUri(const std::string* mime_type,
                    const char* default_mime_type,
                    const std::string& base64) {
  return "data:" + (mime_type ? *mime_type : std::string(default_mime_type)) +
         ";base64," + base64;
}

// The Responses API requires a filename next to `file_data` and infers the
// file type from its EXTENSION, so an extension-less name is rejected — and
// since the part stays in the transcript, every later turn would be too. The
// bridge's document part carries no filename; the file's real path reaches
// the model in the guest's text next to the part.
std::string DocumentFilename(const std::string* mime_type) {
  if (!mime_type || *mime_type == "application/pdf") {
    return "document.pdf";
  }
  base::FilePath::StringType extension;
  if (net::GetPreferredExtensionForMimeType(*mime_type, &extension)) {
    return base::FilePath(FILE_PATH_LITERAL("document"))
        .AddExtension(extension)
        .AsUTF8Unsafe();
  }
  return "document";
}

base::DictValue InputText(std::string text) {
  base::DictValue part;
  part.Set("type", "input_text");
  part.Set("text", std::move(text));
  return part;
}

}  // namespace

std::optional<std::string> ImageUrlForPart(const base::DictValue& part) {
  const std::string* mime_type = part.FindString("mimeType");
  if (const std::string* url = part.FindString("url")) {
    return *url;
  }
  if (const std::string* image = part.FindString("image")) {
    if (IsUrlOrDataUri(*image)) {
      return *image;
    }
    return DataUri(mime_type, "image/png", *image);
  }
  if (const std::string* data = part.FindString("data")) {
    return DataUri(mime_type, "image/png", *data);
  }
  return std::nullopt;
}

base::Value BuildResponsesUserContent(const base::ListValue& parts,
                                      bool accepts_input_file) {
  bool text_only = true;
  for (const auto& part_val : parts) {
    const base::DictValue* part = part_val.GetIfDict();
    const std::string* type = part ? part->FindString("type") : nullptr;
    if (type && (*type == "image" || *type == "document")) {
      text_only = false;
      break;
    }
  }

  if (text_only) {
    std::string combined;
    for (const auto& part_val : parts) {
      const base::DictValue* part = part_val.GetIfDict();
      const std::string* type = part ? part->FindString("type") : nullptr;
      const std::string* text = part ? part->FindString("text") : nullptr;
      if (type && *type == "text" && text) {
        if (!combined.empty()) {
          combined += '\n';
        }
        combined += *text;
      }
    }
    return base::Value(std::move(combined));
  }

  base::ListValue content;
  for (const auto& part_val : parts) {
    const base::DictValue* part = part_val.GetIfDict();
    if (!part) {
      continue;
    }
    const std::string* type = part->FindString("type");
    if (!type) {
      continue;
    }
    const std::string* mime_type = part->FindString("mimeType");

    if (*type == "text") {
      if (const std::string* text = part->FindString("text")) {
        content.Append(InputText(*text));
      }
    } else if (*type == "image") {
      if (std::optional<std::string> url = ImageUrlForPart(*part)) {
        base::DictValue image;
        image.Set("type", "input_image");
        image.Set("image_url", std::move(*url));
        content.Append(std::move(image));
      }
    } else if (*type == "document") {
      const std::string* data = part->FindString("data");
      if (accepts_input_file && data && !data->empty()) {
        base::DictValue file;
        file.Set("type", "input_file");
        file.Set("filename", DocumentFilename(mime_type));
        file.Set("file_data", DataUri(mime_type, "application/pdf", *data));
        content.Append(std::move(file));
      } else {
        content.Append(InputText(DocumentUnavailableMarker(mime_type)));
      }
    }
  }
  return base::Value(std::move(content));
}

}  // namespace nexus
