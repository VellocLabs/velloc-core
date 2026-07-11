#include "custom_browser/browser/nexus/tools/nexus_tool_extract.h"

#include <utility>

#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"

namespace nexus {

namespace {

std::string MakeMissingError(std::string_view name) {
  return base::StrCat({"missing required argument `", name, "`"});
}

std::string MakeWrongTypeError(std::string_view name,
                               std::string_view expected) {
  return base::StrCat({"argument `", name, "` must be ", expected});
}

}  // namespace

// =============================================================================
// std::string
// =============================================================================
//
// Empty-string policy: a required `String` parameter rejects an empty
// value. Almost every tool body would otherwise repeat the same
//   `if (foo.empty()) Reject("invalid_args", "foo must be non-empty")`
// guard — instead the framework rejects up front so the body only runs
// with a usable value. Callers that genuinely want to allow `""` should
// declare the parameter as `std::optional<std::string>` and have the
// LLM omit the key for "no value", rather than passing the empty
// string. Optional-string parameters reach this same Extract via the
// `ToolArgExtractor<std::optional<U>>` wrapper, so passing `key: ""`
// also rejects on the optional path — that's deliberate, "no value"
// belongs on the missing-key path.
base::expected<std::string, std::string>
ToolArgExtractor<std::string>::Extract(const base::DictValue& args,
                                       std::string_view name) {
  const std::string* str = args.FindString(name);
  if (!str) {
    if (args.Find(name) == nullptr) {
      return base::unexpected(MakeMissingError(name));
    }
    return base::unexpected(MakeWrongTypeError(name, "a string"));
  }
  if (str->empty()) {
    return base::unexpected(
        base::StrCat({"argument `", name,
                      "` must be a non-empty string (omit the key entirely "
                      "to convey absence)"}));
  }
  // Both T and E are std::string here, so the implicit `expected(U&&)`
  // constructor is `explicit` and won't accept a bare string. `base::ok`
  // wraps the value into the disambiguating `ok<U>` proxy.
  return base::ok(*str);
}

// =============================================================================
// int / int64_t
// =============================================================================
base::expected<int, std::string> ToolArgExtractor<int>::Extract(
    const base::DictValue& args, std::string_view name) {
  std::optional<int> value = args.FindInt(name);
  if (!value.has_value()) {
    if (args.Find(name) == nullptr) {
      return base::unexpected(MakeMissingError(name));
    }
    return base::unexpected(MakeWrongTypeError(name, "an integer"));
  }
  return *value;
}

base::expected<int64_t, std::string> ToolArgExtractor<int64_t>::Extract(
    const base::DictValue& args, std::string_view name) {
  // base::DictValue stores integers as `int` (32-bit) by default;
  // 64-bit values arrive as a string-encoded number on the wire and are
  // commonly stored as a string field. Try both.
  std::optional<int> as_int = args.FindInt(name);
  if (as_int.has_value()) {
    return static_cast<int64_t>(*as_int);
  }
  const std::string* as_str = args.FindString(name);
  if (as_str) {
    int64_t parsed = 0;
    if (base::StringToInt64(*as_str, &parsed)) {
      return parsed;
    }
    return base::unexpected(MakeWrongTypeError(name, "a 64-bit integer"));
  }
  if (args.Find(name) == nullptr) {
    return base::unexpected(MakeMissingError(name));
  }
  return base::unexpected(MakeWrongTypeError(name, "an integer"));
}

// =============================================================================
// double
// =============================================================================
base::expected<double, std::string> ToolArgExtractor<double>::Extract(
    const base::DictValue& args, std::string_view name) {
  std::optional<double> value = args.FindDouble(name);
  if (value.has_value()) {
    return *value;
  }
  // Allow integer JSON values to be read as doubles — JSON doesn't
  // distinguish "3" from "3.0".
  std::optional<int> as_int = args.FindInt(name);
  if (as_int.has_value()) {
    return static_cast<double>(*as_int);
  }
  if (args.Find(name) == nullptr) {
    return base::unexpected(MakeMissingError(name));
  }
  return base::unexpected(MakeWrongTypeError(name, "a number"));
}

// =============================================================================
// bool
// =============================================================================
base::expected<bool, std::string> ToolArgExtractor<bool>::Extract(
    const base::DictValue& args, std::string_view name) {
  std::optional<bool> value = args.FindBool(name);
  if (!value.has_value()) {
    if (args.Find(name) == nullptr) {
      return base::unexpected(MakeMissingError(name));
    }
    return base::unexpected(MakeWrongTypeError(name, "a boolean"));
  }
  return *value;
}

// =============================================================================
// base::DictValue (escape hatch for nested objects)
// =============================================================================
base::expected<base::DictValue, std::string>
ToolArgExtractor<base::DictValue>::Extract(const base::DictValue& args,
                                           std::string_view name) {
  const base::DictValue* nested = args.FindDict(name);
  if (!nested) {
    if (args.Find(name) == nullptr) {
      return base::unexpected(MakeMissingError(name));
    }
    return base::unexpected(MakeWrongTypeError(name, "an object"));
  }
  return nested->Clone();
}

// =============================================================================
// base::ListValue (escape hatch for nested arrays)
// =============================================================================

base::expected<base::ListValue, std::string>
ToolArgExtractor<base::ListValue>::Extract(const base::DictValue& args,
                                           std::string_view name) {
  const base::ListValue* nested = args.FindList(name);
  if (!nested) {
    if (args.Find(name) == nullptr) {
      return base::unexpected(MakeMissingError(name));
    }
    return base::unexpected(MakeWrongTypeError(name, "an array"));
  }
  return nested->Clone();
}

// =============================================================================
// Session key extraction
// =============================================================================

std::string ExtractSessionKey(const base::DictValue& payload) {
  // Preferred shape: `{ executionContext: { sessionId: "..." } }`. This
  // is what the JS guest's `buildHostedToolExecutionContext` produces.
  if (const base::DictValue* ctx = payload.FindDict("executionContext")) {
    if (const std::string* session_id = ctx->FindString("sessionId")) {
      if (!session_id->empty()) {
        return *session_id;
      }
    }
  }
  // Fallback for callers that pass `sessionId` at the top level (older
  // protocol shape).
  if (const std::string* top_level = payload.FindString("sessionId")) {
    if (!top_level->empty()) {
      return *top_level;
    }
  }
  return "__default__";
}

}  // namespace nexus
