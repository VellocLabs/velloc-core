#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_NEXUS_TOOL_MACROS_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_NEXUS_TOOL_MACROS_H_

// Per-category accessor declaration helpers — what survives of the
// pre-Phase-4 "macro DSL". The original DSL (`NEXUS_BEGIN_TOOLS` /
// `NEXUS_TOOL` / `NEXUS_INPUT` / `NEXUS_END_TOOL` / `NEXUS_END_TOOLS`)
// was the hand-written tool registration path; it was retired when
// codegen took over (see plan giggly-watching-pie.md, Phase 4-6).
//
// What remains is one piece of glue the codegen-emitted dispatcher
// still needs: a way to descend from `ToolSession*` to a per-category
// instance like `BrowserTool*`. That glue is a primary template
// `CategoryAccessor<T>` declared here, with one specialization per
// category injected via the `NEXUS_DECLARE_CATEGORY` /
// `NEXUS_DEFINE_CATEGORY` pair.
//
// Usage — in the category's header (e.g. browser_tool.h), after the
// class definition:
//
//   NEXUS_DECLARE_CATEGORY(nexus::BrowserTool);
//
// And in the matching .cc (after `#include "tool_session.h"` so the
// `ToolSession::browser_tool()` accessor is fully visible):
//
//   NEXUS_DEFINE_CATEGORY(nexus::BrowserTool, browser_tool);
//
// The two-step split exists to break a circular include — the header
// can only forward-declare `ToolSession`, while the .cc needs the
// full type to call the accessor method.

namespace nexus {

class ToolSession;

namespace detail {

// Primary template; per-category specializations injected by
// `NEXUS_DECLARE_CATEGORY` (declaration) and `NEXUS_DEFINE_CATEGORY`
// (definition). The codegen-emitted dispatcher in
// nexus_tools_registry.gen.cc invokes
// `CategoryAccessor<Category>::Get(session)` to obtain the per-session
// category instance before forwarding the typed input to the
// `Run<Name>` virtual.
template <typename Category>
struct CategoryAccessor;

}  // namespace detail
}  // namespace nexus

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

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_NEXUS_TOOL_MACROS_H_
