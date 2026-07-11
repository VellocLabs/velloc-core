#include "custom_browser/browser/nexus/tools/tool_catalog.h"

#include "base/no_destructor.h"
#include "custom_browser/browser/nexus/tools/browser_tool/browser_tool.h"
#include "custom_browser/browser/nexus/tools/nexus_tools_registry.gen.h"

namespace nexus {

ToolCatalog::ToolCatalog() {
  // Each per-category class registers its NEXUS_TOOL blocks via its
  // static DefineTools method. Add a line per new category.
  BrowserTool::DefineTools(registry_);
  // SkillTool::DefineTools(registry_);   // future

  // Generated registrations from *.tool.json declarations. The two
  // pipelines coexist while the macro→declarative migration is in
  // progress (see plan giggly-watching-pie.md). Generated entries
  // are appended after the macro entries; if a tool name clashes,
  // the generated one wins (last-in-the-vector wins on the
  // dispatcher lookup) — but that's a misconfiguration we'll catch
  // in Phase 4 when the macros are removed.
  ::nexus::generated::PopulateRegistry(registry_);

  schemas_ = registry_.CollectSchemas();

  // Copy entries out so adapters see a stable read-only view. The
  // dispatcher in each entry is a `base::RepeatingCallback`, which is
  // copyable; the underlying state is shared.
  for (const ToolEntry& entry : registry_.entries()) {
    entries_.push_back(ToolEntry{entry.schema, entry.dispatcher});
  }
}

// Out-of-line definition required by chromium-style for a class with
// non-trivial members (the registry / schemas / entries vectors hold
// strings + callbacks). The body is `= default;` because every member
// cleans itself up.
ToolCatalog::~ToolCatalog() = default;

// static
const ToolCatalog& ToolCatalog::Get() {
  static const base::NoDestructor<ToolCatalog> instance;
  return *instance;
}

}  // namespace nexus
