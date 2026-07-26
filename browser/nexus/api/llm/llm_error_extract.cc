// Copyright 2026 The Custom Browser Authors. All rights reserved.

#include "custom_browser/browser/nexus/api/llm/llm_error_extract.h"

#include <optional>
#include <string>
#include <string_view>

#include "base/json/json_reader.h"
#include "base/strings/string_number_conversions.h"
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

// Parses the delta-seconds form of Retry-After. Returns nullopt for the
// HTTP-date form, garbage, or a negative value.
std::optional<int> ParseRetryAfterSeconds(std::string_view raw) {
  if (raw.empty()) {
    return std::nullopt;
  }
  int seconds = 0;
  if (!base::StringToInt(raw, &seconds) || seconds < 0) {
    return std::nullopt;
  }
  return seconds;
}

}  // namespace

std::string BuildProviderErrorMessage(int http_status,
                                      std::string_view retry_after_header,
                                      std::string_view message) {
  std::string out;
  // Only real HTTP status codes; providers pass -1 / 0 when the request never
  // got a response (net error, no headers).
  const bool has_status = http_status >= 100 && http_status <= 599;
  if (has_status) {
    out = "HTTP " + base::NumberToString(http_status);
    if (!message.empty()) {
      out += ": ";
      out.append(message);
    }
  } else {
    out.assign(message);
  }

  if (std::optional<int> retry_after =
          ParseRetryAfterSeconds(retry_after_header)) {
    if (!out.empty()) {
      out += " ";
    }
    out += "(retry-after: " + base::NumberToString(*retry_after) + "s)";
  }
  return out;
}

ParsedProviderError ParseProviderErrorMessage(std::string_view message) {
  ParsedProviderError out;

  // Suffix: "(retry-after: <n>s)" at the very END of the string — the only
  // place BuildProviderErrorMessage puts it. An occurrence elsewhere (e.g.
  // quoted inside a provider body) is not the canonical suffix and is
  // ignored; digits containing anything non-numeric fail StringToInt.
  constexpr std::string_view kRetrySuffixOpen = "(retry-after: ";
  if (message.ends_with("s)")) {
    const size_t open = message.rfind(kRetrySuffixOpen);
    if (open != std::string_view::npos) {
      std::string_view digits =
          message.substr(open + kRetrySuffixOpen.size(),
                         message.size() - (open + kRetrySuffixOpen.size()) - 2);
      int seconds = 0;
      if (!digits.empty() && base::StringToInt(digits, &seconds) &&
          seconds >= 0) {
        out.retry_after_seconds = seconds;
      }
    }
  }

  // Prefix: "HTTP <3 digits>" at the very START, followed by ':', ' ' or
  // end-of-string — exactly the shapes the formatter emits ("HTTP 500",
  // "HTTP 429: msg", "HTTP 503 (retry-after: 5s)").
  constexpr std::string_view kHttpPrefix = "HTTP ";
  if (message.starts_with(kHttpPrefix)) {
    const std::string_view rest = message.substr(kHttpPrefix.size());
    if (rest.size() >= 3) {
      const char next = rest.size() > 3 ? rest[3] : '\0';
      if (next == '\0' || next == ':' || next == ' ') {
        int status = 0;
        if (base::StringToInt(rest.substr(0, 3), &status) && status >= 100 &&
            status <= 599) {
          out.http_status = status;
        }
      }
    }
  }

  return out;
}

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
