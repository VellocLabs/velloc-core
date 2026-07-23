// Copyright 2026 The Custom Browser Authors. All rights reserved.

#include "custom_browser/browser/nexus/api/llm/llm_error_extract.h"

#include <optional>
#include <string>
#include <string_view>

#include "base/json/json_reader.h"
#include "base/values.h"

namespace nexus {

namespace {

// Error-shape extraction shared by the raw-body and SSE-frame paths.
// Returns empty when `parsed` carries no recognizable error field.
std::string ExtractFromDict(const base::DictValue& parsed) {
  if (const base::DictValue* err = parsed.FindDict("error")) {
    if (const std::string* msg = err->FindString("message")) {
      return *msg;
    }
  }
  if (const std::string* err = parsed.FindString("error")) {
    return *err;
  }
  if (const std::string* detail = parsed.FindString("detail")) {
    return *detail;
  }
  if (const std::string* message = parsed.FindString("message")) {
    return *message;
  }
  return std::string();
}

}  // namespace

std::string ExtractApiErrorMessage(const std::string& body) {
  // Fast path: the whole body is a JSON error object (the non-stream
  // error shape every major backend uses).
  if (std::optional<base::DictValue> parsed =
          base::JSONReader::ReadDict(body, base::JSON_PARSE_RFC)) {
    if (std::string msg = ExtractFromDict(*parsed); !msg.empty()) {
      return msg;
    }
  }

  // SSE-frame path: some gateways deliver error objects inside `data:`
  // events even for failed requests. Scan each event payload; the first
  // recognizable error object wins. A normal chat stream yields nothing
  // — its data payloads carry `choices`/`delta`, not an error shape, and
  // `[DONE]` isn't JSON.
  constexpr std::string_view kDataPrefix = "data:";
  for (std::string_view rest(body); !rest.empty();) {
    const std::string_view::size_type line_end = rest.find('\n');
    std::string_view line =
        line_end == std::string_view::npos ? rest : rest.substr(0, line_end);
    rest = line_end == std::string_view::npos ? std::string_view()
                                              : rest.substr(line_end + 1);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);  // CRLF-terminated streams.
    }
    if (!line.starts_with(kDataPrefix)) {
      continue;
    }
    std::string_view payload = line.substr(kDataPrefix.size());
    if (!payload.empty() && payload.front() == ' ') {
      payload.remove_prefix(1);  // SSE's single optional space after ':'.
    }
    if (std::optional<base::DictValue> parsed =
            base::JSONReader::ReadDict(payload, base::JSON_PARSE_RFC)) {
      if (std::string msg = ExtractFromDict(*parsed); !msg.empty()) {
        return msg;
      }
    }
  }
  return std::string();
}

}  // namespace nexus
