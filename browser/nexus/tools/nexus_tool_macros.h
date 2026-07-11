#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_NEXUS_TOOL_MACROS_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_NEXUS_TOOL_MACROS_H_

#include <cstddef>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "base/check_op.h"
#include "base/functional/bind.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_callback.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_extract.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_registry.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_schema.h"

// Macro DSL for declaring nexus C++ tools, **session-routed**.
//
// `NEXUS_TOOL` blocks live next to the implementations they describe.
// Each block declares a tool's schema (the LLM-facing name +
// description + parameter shapes) and binds a typed dispatcher that
// the framework resolves to a per-session category instance at
// dispatch time.
//
// One block per tool, in the per-category class's `DefineTools` body:
//
//   void BrowserTool::DefineTools(::nexus::ToolRegistry& tools) {
//     NEXUS_BEGIN_TOOLS(BrowserTool, tools)
//
//       NEXUS_TOOL(example_navigate, "Navigate the tab to a URL.")
//         NEXUS_INPUT_OPTIONAL (Integer, tab_id, "Optional. Active tab if absent.")
//         NEXUS_INPUT          (String,  url,    "Target URL.")
//       NEXUS_END_TOOL(Object)
//
//     NEXUS_END_TOOLS()
//   }
//
// Tool methods live as instance methods on the category class
// (e.g. `ExampleTool::example_navigate`). The framework derives the
// dispatcher from the method-pointer signature: per-request, it
//   1. asks the adapter for the matching `ToolSession` (keyed by
//      `executionContext.sessionId`);
//   2. descends to the per-category instance via
//      `CategoryAccessor<Category>::Get(session)`;
//   3. extracts every declared input from the JSON args dict using
//      `ToolArgExtractor<T>` and rejects with `invalid_args` on any
//      failure;
//   4. invokes the typed method with the extracted args + a fresh
//      `ToolCallback`.

namespace nexus::detail {

// =============================================================================
// CategoryAccessor — descends from `ToolSession` to a per-category
// instance member.
//
// Each category class (BrowserTool, future SkillTool, …) provides a
// specialization in its own header. Example (in browser_tool.h):
//
//   template <>
//   struct nexus::detail::CategoryAccessor<BrowserTool> {
//     static BrowserTool* Get(ToolSession* session) {
//       return session ? &session->browser_tool() : nullptr;
//     }
//   };
//
// The macro DSL invokes `CategoryAccessor<Category>::Get(session)` at
// dispatch time to find the per-session instance to call into.
// =============================================================================

template <typename Category>
struct CategoryAccessor;  // primary; per-category specialization required.

// =============================================================================
// Compile-time trait: arity of a tool method (data args, excluding the
// trailing `ToolCallback`).
//
// Note: we deduce on `void (Self::*)(AllArgs...)` (pure trailing pack)
// rather than `void (Self::*)(DataArgs..., ToolCallback)`. The latter
// pattern — a parameter pack followed by a fixed type — is a known
// clang partial-specialization deduction limitation; the all-args
// variant deduces cleanly. We then static_assert the last element is
// `ToolCallback` and report `value = sizeof...(AllArgs) - 1`.
// =============================================================================

template <typename T>
struct TypedToolArity;

template <typename Self, typename... AllArgs>
struct TypedToolArity<void (Self::*)(AllArgs...)> {
 private:
  static_assert(sizeof...(AllArgs) >= 1,
                "Tool method must take at least ToolCallback");
  using LastArg =
      std::tuple_element_t<sizeof...(AllArgs) - 1, std::tuple<AllArgs...>>;
  static_assert(std::is_same_v<LastArg, ToolCallback>,
                "Tool method's last parameter must be ToolCallback");

 public:
  static constexpr std::size_t value = sizeof...(AllArgs) - 1;
};

// =============================================================================
// Recursive extraction + invocation. Pulls each data arg out of the
// JSON dict via `ToolArgExtractor<T>`, captures the first error (in
// declaration order), and on success invokes the method on the
// per-category instance with the extracted typed values + the trailing
// `ToolCallback`.
// =============================================================================

template <typename Category, typename... AllArgs, std::size_t... DataIs>
void ExtractAndInvokeImpl(
    Category* category,
    void (Category::*method)(AllArgs...),
    const std::vector<std::string>& names,
    base::DictValue args,
    ToolCallback done,
    std::index_sequence<DataIs...>) {
  using AllArgsTuple = std::tuple<AllArgs...>;

  // Pull every declared data arg into the matching C++ type.
  std::tuple<base::expected<
      std::tuple_element_t<DataIs, AllArgsTuple>, std::string>...>
      extracted{ToolArgExtractor<
          std::tuple_element_t<DataIs, AllArgsTuple>>::Extract(args,
                                                               names[DataIs])...};

  // Find the first failed extraction (in declaration order).
  std::optional<std::string> first_error;
  std::apply(
      [&first_error](auto&... result) {
        ((!first_error.has_value() && !result.has_value()
              ? (first_error = std::move(result.error()), 0)
              : 0),
         ...);
      },
      extracted);
  if (first_error.has_value()) {
    // Append the full declared parameter list so the LLM can recover
    // in-place without re-reading the schema. Empty-args calls like
    // `example_navigate({})` are the most common failure mode and
    // benefit the most from seeing "(expected: tab_id?, url)" inline.
    std::string param_hint = " (expected: ";
    for (size_t i = 0; i < names.size(); ++i) {
      if (i > 0) {
        param_hint += ", ";
      }
      param_hint += names[i];
    }
    param_hint += ")";
    *first_error += param_hint;
    std::move(done).Reject("invalid_args", std::move(*first_error));
    return;
  }

  // All extractions succeeded — invoke the typed method on the category.
  (category->*method)(std::move(std::get<DataIs>(extracted).value())...,
                      std::move(done));
}

// =============================================================================
// `BindTypedCategoryMethod` — turns a typed member function pointer +
// parameter-name list into a session-routed `ToolDispatcher`.
//
// At dispatch time the dispatcher receives `(ToolSession*, args, done)`,
// asks `CategoryAccessor<Category>::Get(session)` for the per-session
// instance, then runs the typed extraction + method invocation.
//
// If the session is null (or the accessor returns null — which would be
// a bug indicating the session lacks the requested category), the
// dispatcher rejects with `session_missing` so the LLM gets an
// actionable error instead of a hang.
// =============================================================================

template <typename Category, typename... AllArgs>
ToolDispatcher BindTypedCategoryMethod(
    void (Category::*method)(AllArgs...),
    std::vector<std::string> names) {
  constexpr std::size_t kDataArity = TypedToolArity<decltype(method)>::value;
  CHECK_EQ(names.size(), kDataArity)
      << "Tool parameter count mismatch: macro declared " << names.size()
      << " inputs but method takes " << kDataArity
      << " data args. Did you forget a NEXUS_INPUT or include an extra one?";

  return base::BindRepeating(
      [](void (Category::*m)(AllArgs...), std::vector<std::string> n,
         ToolSession* session, base::DictValue args, ToolCallback done) {
        Category* category = CategoryAccessor<Category>::Get(session);
        if (!category) {
          std::move(done).Reject(
              "session_missing",
              "session is missing the requested tool category");
          return;
        }
        ExtractAndInvokeImpl<Category, AllArgs...>(
            category, m, n, std::move(args), std::move(done),
            std::make_index_sequence<sizeof...(AllArgs) - 1>{});
      },
      method, std::move(names));
}

}  // namespace nexus::detail

// =============================================================================
// Category-accessor declaration helpers
// =============================================================================
//
// Each per-category class needs a `CategoryAccessor<Cls>` specialization so
// the macro DSL can descend from `ToolSession` to the per-category instance
// at dispatch time. The specialization is split across header + .cc to
// avoid circular includes (the header forward-declares `ToolSession`; the
// .cc includes `tool_session.h` for the accessor body).
//
// Wrapping that split in macros means a category file declares its
// accessor in two short lines instead of writing the boilerplate by
// hand.
//
// Usage — in browser_tool.h, after the class definition:
//
//   NEXUS_DECLARE_CATEGORY(BrowserTool);
//
// And in browser_tool.cc, after `#include "tool_session.h"`:
//
//   NEXUS_DEFINE_CATEGORY(BrowserTool, browser_tool);
//
// `accessor_method` is the public accessor on `ToolSession` that
// returns the per-category member by reference (e.g.
// `ToolSession::browser_tool()` returns `BrowserTool&`). The macro
// emits a `nullptr`-safe dispatcher that returns a pointer to that
// reference (or `nullptr` if `session` is itself null — which would
// indicate a bug, but the framework rejects with `session_missing`
// rather than crashing).

#define NEXUS_DECLARE_CATEGORY(Class)                                       \
  namespace nexus::detail {                                                 \
  template <>                                                               \
  struct CategoryAccessor<Class> {                                          \
    static Class* Get(::nexus::ToolSession* session);                       \
  };                                                                        \
  } /* namespace nexus::detail */                                           \
  static_assert(true, "force trailing semicolon")

#define NEXUS_DEFINE_CATEGORY(Class, accessor_method)                       \
  namespace nexus::detail {                                                 \
  Class* CategoryAccessor<Class>::Get(::nexus::ToolSession* session) {      \
    return session ? &session->accessor_method() : nullptr;                 \
  }                                                                         \
  } /* namespace nexus::detail */                                           \
  static_assert(true, "force trailing semicolon")

// =============================================================================
// Block boundaries
// =============================================================================

// Open the tool-definition block. `Class` is the per-category class
// (BrowserTool, future SkillTool, …) — the macro captures its type
// for `NEXUS_END_TOOL`'s session-routed bind. `tools_ref` is the
// `ToolRegistry&` parameter the static `DefineTools` receives.
#define NEXUS_BEGIN_TOOLS(Class, tools_ref)                          \
  using _NexusToolHostT = Class;                                     \
  ::nexus::ToolRegistry& _nexus_tool_registry = (tools_ref);

// Close the block. Empty for now — the macro is symmetric with
// `NEXUS_BEGIN_TOOLS` and reserves the space for future cross-tool
// validation (e.g. duplicate-name detection).
#define NEXUS_END_TOOLS() /* no-op */

// =============================================================================
// Per-tool block
// =============================================================================

// Open a tool. `name` is an unquoted identifier matching the member
// function on the category class (e.g. `example_navigate`); the macro
// emits `#name` for the schema and captures
// `&_NexusToolHostT::name` so `NEXUS_END_TOOL` doesn't need to repeat
// it.
//
// `desc` is a string literal shipped to the LLM as the tool's
// top-level description. Per-parameter prose lives in `NEXUS_INPUT`'s
// `desc` argument — the JSON Schema's `description` for that property.
#define NEXUS_TOOL(name, desc)                                       \
  do {                                                               \
    auto _nexus_tool_builder =                                       \
        _nexus_tool_registry.Tool(#name, desc);                      \
    std::vector<std::string> _nexus_tool_param_names;                \
    auto _nexus_tool_method = &_NexusToolHostT::name;

#define NEXUS_INPUT(kind, param_name, desc)                          \
    _nexus_tool_builder.Required(#param_name, desc,                  \
                                 ::nexus::JsonSchemaKind::k##kind);  \
    _nexus_tool_param_names.emplace_back(#param_name);

#define NEXUS_INPUT_OPTIONAL(kind, param_name, desc)                 \
    _nexus_tool_builder.Optional(#param_name, desc,                  \
                                 ::nexus::JsonSchemaKind::k##kind);  \
    _nexus_tool_param_names.emplace_back(#param_name);

// Close a tool. Records the output kind in the schema (informational —
// the wire format does not currently surface it but it's reserved for
// future structured-output hints) and binds the dispatcher via
// `BindTypedCategoryMethod`. The dispatcher derives the data-arg count
// from the method-pointer's signature, packs the runtime names list,
// and per-request:
//   1. asks the framework for the per-session category instance
//      (via `CategoryAccessor<_NexusToolHostT>::Get(session)`);
//   2. extracts the typed args from the JSON dict;
//   3. invokes the method.
#define NEXUS_END_TOOL(output_kind)                                          \
    _nexus_tool_builder.Returns(::nexus::JsonSchemaKind::k##output_kind);    \
    std::move(_nexus_tool_builder)                                           \
        .Bind(::nexus::detail::BindTypedCategoryMethod<_NexusToolHostT>(     \
            _nexus_tool_method,                                              \
            std::move(_nexus_tool_param_names)));                            \
  } while (0);

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_NEXUS_TOOL_MACROS_H_
