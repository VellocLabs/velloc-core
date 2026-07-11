// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/utility/nexus/media/notebook_processor.h"

#include <string>
#include <utility>

#include "base/base64.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "custom_browser/utility/nexus/media/media_limits.h"

namespace nexus {
namespace {

std::string ReadTextFromValue(const base::Value* value) {
  if (!value) return std::string();
  if (value->is_string()) return value->GetString();
  const base::ListValue* list = value->GetIfList();
  if (!list) return std::string();
  std::string out;
  for (const auto& item : *list) {
    if (item.is_string()) out.append(item.GetString());
  }
  return out;
}

std::string NotebookCellId(const base::DictValue& cell, int index) {
  const std::string* id = cell.FindString("id");
  if (id && !id->empty()) return *id;
  return base::StrCat({"cell-", base::NumberToString(index)});
}

std::string TruncateOutput(std::string text) {
  if (text.size() <= media_limits::kNotebookOutputThreshold) return text;
  text.resize(media_limits::kNotebookOutputThreshold);
  text.append("\n[output truncated]");
  return text;
}

void AppendOutputParts(const base::DictValue& output,
                       mojom::MediaReadResult* result) {
  const std::string* output_type = output.FindString("output_type");
  if (!output_type) return;
  if (*output_type == "stream") {
    result->parts.push_back(
        MakeTextMediaPart("\n" + TruncateOutput(ReadTextFromValue(output.Find("text")))));
    return;
  }
  if (*output_type == "error") {
    std::string text;
    if (const std::string* ename = output.FindString("ename")) text.append(*ename);
    if (const std::string* evalue = output.FindString("evalue")) {
      if (!text.empty()) text.append(": ");
      text.append(*evalue);
    }
    if (const base::ListValue* traceback = output.FindList("traceback")) {
      for (const auto& line : *traceback) {
        if (line.is_string()) {
          text.push_back('\n');
          text.append(line.GetString());
        }
      }
    }
    result->parts.push_back(MakeTextMediaPart("\n" + TruncateOutput(std::move(text))));
    return;
  }
  if (*output_type != "execute_result" && *output_type != "display_data") return;
  const base::DictValue* data = output.FindDict("data");
  if (!data) return;
  const std::string plain = ReadTextFromValue(data->Find("text/plain"));
  if (!plain.empty()) result->parts.push_back(MakeTextMediaPart("\n" + TruncateOutput(plain)));
  const std::string* image_data = data->FindString("image/png");
  std::string mime = "image/png";
  if (!image_data) {
    image_data = data->FindString("image/jpeg");
    mime = "image/jpeg";
  }
  if (image_data) {
    std::string compact;
    base::RemoveChars(*image_data, " \t\r\n", &compact);
    std::optional<std::vector<uint8_t>> decoded = base::Base64Decode(compact);
    if (decoded.has_value()) {
      result->parts.push_back(MakeBinaryMediaPart("image", mime, std::move(*decoded)));
    }
  }
}

}  // namespace

MediaReadOutcome ReadNotebookMedia(const base::FilePath& path) {
  std::string contents;
  if (!base::ReadFileToString(path, &contents)) {
    return MakeMediaError("read_failed", "failed to read notebook file");
  }
  std::optional<base::Value> parsed =
      base::JSONReader::Read(contents, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    return MakeMediaError("invalid_notebook", "failed to parse notebook JSON");
  }
  const base::DictValue& notebook = parsed->GetDict();
  const base::ListValue* cells = notebook.FindList("cells");
  if (!cells) {
    return MakeMediaError("invalid_notebook", "notebook JSON does not contain a cells array");
  }
  std::string language = "python";
  if (const base::DictValue* metadata = notebook.FindDict("metadata")) {
    if (const base::DictValue* language_info = metadata->FindDict("language_info")) {
      if (const std::string* name = language_info->FindString("name")) language = *name;
    }
  }
  auto result = mojom::MediaReadResult::New();
  result->full_text_for_cache = contents;
  size_t text_bytes = 0;
  int index = 0;
  for (const auto& cell_value : *cells) {
    const base::DictValue* cell = cell_value.GetIfDict();
    if (!cell) {
      ++index;
      continue;
    }
    const std::string* cell_type = cell->FindString("cell_type");
    const std::string type = cell_type ? *cell_type : std::string("code");
    const std::string source = ReadTextFromValue(cell->Find("source"));
    const std::string id = NotebookCellId(*cell, index);
    std::string cell_text = base::StrCat({"<cell id=\"", id, "\">"});
    if (type != "code") cell_text.append(base::StrCat({"<cell_type>", type, "</cell_type>"}));
    else if (language != "python") cell_text.append(base::StrCat({"<language>", language, "</language>"}));
    cell_text.append(source);
    cell_text.append(base::StrCat({"</cell id=\"", id, "\">"}));
    text_bytes += cell_text.size();
    if (text_bytes > media_limits::kReadResponseByteCap) {
      result->parts.push_back(MakeTextMediaPart("[notebook content truncated]"));
      break;
    }
    result->parts.push_back(MakeTextMediaPart(std::move(cell_text)));
    if (type == "code") {
      if (const base::ListValue* outputs = cell->FindList("outputs")) {
        for (const auto& output_value : *outputs) {
          if (const base::DictValue* output = output_value.GetIfDict()) {
            AppendOutputParts(*output, result.get());
          }
        }
      }
    }
    ++index;
  }
  MediaReadOutcome outcome;
  outcome.ok = true;
  outcome.result = std::move(result);
  return outcome;
}

}  // namespace nexus
