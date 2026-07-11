#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_TOOL_CATALOG_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_TOOL_CATALOG_H_

#include <vector>

#include "base/containers/span.h"
#include "base/no_destructor.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_registry.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_schema.h"

namespace nexus {

// `ToolCatalog` — process-global, lazily-constructed singleton. Holds
// the static metadata (`schemas_`) and the per-tool dispatcher
// trampolines (`entries_`) that every `ToolsAdapter` instance shares.
//
// Built once on first access via `Get()`. The constructor calls each
// per-category class's static `DefineTools(registry)` (currently just
// `BrowserTool::DefineTools`; future categories add lines in the ctor)
// to populate the registry.
//
// The dispatchers reference category-class member function pointers
// (e.g. `&ExampleTool::example_navigate`); at dispatch time the
// adapter supplies a `ToolSession*` and the dispatcher's inner
// trampoline uses `CategoryAccessor<Category>::Get(session)` to descend
// to the per-session instance before invoking the typed method.
class ToolCatalog {
 public:
  // base::NoDestructor singleton; constructed on first call.
  static const ToolCatalog& Get();

  // Schemas exposed to `nexus_host_manifest.cc` for the
  // `manifest.hostedTools` array shipped to the LLM.
  base::span<const ToolSchema> schemas() const { return schemas_; }

  // Entries used by `ToolsAdapter::RegisterCatalogDispatchers` — each
  // entry's `dispatcher` is a session-routed `ToolDispatcher` ready to
  // be wrapped in a bridge handler.
  const std::vector<ToolEntry>& entries() const { return entries_; }

 private:
  // `base::NoDestructor` constructs `ToolCatalog` on first `Get()` —
  // it needs access to the private constructor.
  friend class base::NoDestructor<ToolCatalog>;

  ToolCatalog();
  ~ToolCatalog();
  ToolCatalog(const ToolCatalog&) = delete;
  ToolCatalog& operator=(const ToolCatalog&) = delete;

  ToolRegistry registry_;
  std::vector<ToolSchema> schemas_;
  std::vector<ToolEntry> entries_;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_TOOL_CATALOG_H_
