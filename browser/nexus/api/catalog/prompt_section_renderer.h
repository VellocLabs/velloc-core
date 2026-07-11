#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_CATALOG_PROMPT_SECTION_RENDERER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_CATALOG_PROMPT_SECTION_RENDERER_H_

#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace nexus {

// Renders a named system-prompt section to text. Direct port of the
// pre-Phase-12 `getUsingYourToolsSection` function from
// `nexus_web/src/constants/systemPromptToolSection.ts`. The C++ side is
// the new source of truth for browser-tool-name knowledge: instead of
// the TS `BROWSER_TOOL_NAMES` hardcoded set, the renderer walks
// `ToolCatalog::Get().schemas()` to determine "is this an active
// browser tool?" — a tool is treated as a browser tool iff it's in the
// catalog AND its name is not one of the well-known file tools
// (Read / Write / Edit).
//
// `name` selects the section. Today only "using-your-tools" is
// supported; passing an unknown name returns an empty string.
//
// `enabled_tool_names` is the set of tool names the JS-side runtime
// considers enabled this turn (mirrors the TS `enabledTools` Set).
//
// Returns the composed prose. The output is intended to be substituted
// verbatim into a `{{TOOL_CATALOG}}` placeholder in the .md template.
//
// Pure / stateless / synchronous — safe to call from any thread that
// has read access to `ToolCatalog::Get()` (which is process-global and
// initialized exactly once).
std::string RenderPromptSection(std::string_view name,
                                const std::vector<std::string>&
                                    enabled_tool_names);

// Names of every section the renderer knows about. Used by the
// `listPromptSections` API action.
std::vector<std::string> ListPromptSectionNames();

// True iff `name` is a section the renderer recognizes (even if it would
// render empty for the current tool set, e.g. "using-your-tools" with no
// enabled tools). Lets the holder resolver's DCHECK tell an unknown/misnamed
// holder apart from a known-but-gated-empty one.
bool IsKnownPromptSection(std::string_view name);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_CATALOG_PROMPT_SECTION_RENDERER_H_
