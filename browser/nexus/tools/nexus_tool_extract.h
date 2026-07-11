#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_NEXUS_TOOL_EXTRACT_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_NEXUS_TOOL_EXTRACT_H_

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/types/expected.h"
#include "base/values.h"

// Typed argument extraction for the macro-DSL tool framework.
//
// The framework-generated dispatcher (see `BindTypedToolMethod` in
// `nexus_tool_macros.h`) decodes the JSON args dict into the method's
// declared C++ parameter types via these templates. On any failure
// (missing required key, wrong type) the dispatcher automatically
// rejects the request with a `tool_error` / "invalid_args" code, so
// tool bodies never see a half-validated dict — they get a typed value
// they can use directly:
//
//     void example_create_tab(std::string url,
//                             std::optional<int> window_id,
//                             ToolCallback done);
//
// Specializations live for: std::string, int, int64_t, double, bool,
// base::DictValue/base::ListValue (raw nested containers for advanced inputs),
// and
// std::optional<T> for any of the above.
//
// `std::optional<T>` is the contract for a parameter declared with
// `NEXUS_INPUT_OPTIONAL` — missing key produces `std::nullopt`, present
// key flows through the inner extractor.

namespace nexus {

template <typename T>
struct ToolArgExtractor;

template <>
struct ToolArgExtractor<std::string> {
  static base::expected<std::string, std::string> Extract(
      const base::DictValue& args, std::string_view name);
};

template <>
struct ToolArgExtractor<int> {
  static base::expected<int, std::string> Extract(
      const base::DictValue& args, std::string_view name);
};

template <>
struct ToolArgExtractor<int64_t> {
  static base::expected<int64_t, std::string> Extract(
      const base::DictValue& args, std::string_view name);
};

template <>
struct ToolArgExtractor<double> {
  static base::expected<double, std::string> Extract(
      const base::DictValue& args, std::string_view name);
};

template <>
struct ToolArgExtractor<bool> {
  static base::expected<bool, std::string> Extract(
      const base::DictValue& args, std::string_view name);
};

// Pass-through for tool methods that take a raw nested dict (escape
// hatch for parameters whose schema is too complex to express via the
// kind enum). The body validates the inner shape itself.
template <>
struct ToolArgExtractor<base::DictValue> {
  static base::expected<base::DictValue, std::string> Extract(
      const base::DictValue& args, std::string_view name);
};

// Pass-through for tools that own their own array item validation. Used by
// browser_actions so the native tool-call envelope stays one tool while the
// action list keeps Browser Use's internal batch shape.
template <>
struct ToolArgExtractor<base::ListValue> {
  static base::expected<base::ListValue, std::string> Extract(
      const base::DictValue& args, std::string_view name);
};

// std::optional<T> — missing key produces nullopt, present key
// dispatches to the inner extractor. Errors from the inner extractor
// surface as the optional's error.
template <typename U>
struct ToolArgExtractor<std::optional<U>> {
  static base::expected<std::optional<U>, std::string> Extract(
      const base::DictValue& args, std::string_view name) {
    if (args.Find(name) == nullptr) {
      return std::optional<U>{};
    }
    base::expected<U, std::string> inner =
        ToolArgExtractor<U>::Extract(args, name);
    if (!inner.has_value()) {
      return base::unexpected(std::move(inner).error());
    }
    return std::optional<U>{std::move(inner).value()};
  }
};

// =============================================================================
// Session key extraction
// =============================================================================
//
// Reads `payload.executionContext.sessionId` (the convention the JS
// guest's `buildHostedToolExecutionContext` already populates — see
// `nexus_web/src/tools/glic/hostedTools.ts`). Falls back to the literal
// `"__default__"` if the field is absent or empty so single-session
// callers still get a stable bucket.
//
// Used by `ToolsAdapter`'s session-routing dispatchers to look up (or
// lazily create) the matching `ToolSession`.
std::string ExtractSessionKey(const base::DictValue& payload);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_NEXUS_TOOL_EXTRACT_H_
