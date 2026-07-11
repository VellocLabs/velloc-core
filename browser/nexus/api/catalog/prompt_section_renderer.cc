#include "custom_browser/browser/nexus/api/catalog/prompt_section_renderer.h"

#include <set>
#include <string>
#include <string_view>
#include <vector>

// Phase 13.2: cross-cutting native-tool prose (`{{system-prompt-tools}}`
// placeholder). Source-of-truth is `tools/declarations/system-prompt-
// tools.md`; codegen embeds the file verbatim into the constexpr
// `kSystemPromptToolsContent` string this header exposes.
#include "custom_browser/browser/nexus/tools/system_prompt_tools.gen.h"
// Phase 13.4 / 13.8 / 13.9: catalog-frame strings for the
// `{{TOOL_CATALOG}}` placeholder. Source-of-truth JSON at
// `tools/declarations/tool-catalog-frame.json` is a thin manifest of
// `@file:` references; codegen embeds each referenced .md verbatim as
// a constexpr string_view (`kBrowserToolsContent`,
// `kFileShellToolsContent`, `kTaskTrackingContent`, `kSkillsContent`,
// `kParallelismContent`) plus two wire-name arrays so the renderer
// can gate the browser / file-shell sections on "any tool in that
// category enabled".
#include "custom_browser/browser/nexus/tools/tool_catalog_frame.gen.h"

namespace nexus {

namespace {

using ::nexus::generated::kBrowserToolsContent;
using ::nexus::generated::kBrowserToolWireNames;
using ::nexus::generated::kFileShellToolsContent;
using ::nexus::generated::kFileShellToolWireNames;
using ::nexus::generated::kTaskTrackingContent;
using ::nexus::generated::kSkillsContent;
using ::nexus::generated::kParallelismContent;

// JS-only tool wire-names. These tools live in nexus_web/src/tools/ and
// have no *.tool.json declarations, so the catalog-frame wire-name
// arrays don't cover them — they're checked directly against the JS-
// supplied enabled-tool set. If they ever get C++ declarations, drop
// these constants and put them in the relevant category.
constexpr std::string_view kSkillName = "Skill";
constexpr std::string_view kTodoWriteName = "TodoWrite";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// True iff any wire name in `names` is present in `enabled`. Used by
// the browser / file-shell sections to decide whether to emit at all.
bool AnyEnabled(
    const auto& names,
    const std::set<std::string_view>& enabled) {
  for (std::string_view name : names) {
    if (enabled.find(name) != enabled.end()) {
      return true;
    }
  }
  return false;
}

// Each `Build<X>Section` is now a one-liner: check the section's
// gating condition, emit its constexpr content verbatim. The .md
// files behind each constant carry their own "## ..." headers and
// per-tool bullets — no per-section composition lives in C++ any more.

std::string BuildBrowserToolsSection(
    const std::set<std::string_view>& enabled) {
  if (!AnyEnabled(kBrowserToolWireNames, enabled)) {
    return std::string();
  }
  return std::string(kBrowserToolsContent);
}

std::string BuildFileShellSection(
    const std::set<std::string_view>& enabled) {
  if (!AnyEnabled(kFileShellToolWireNames, enabled)) {
    return std::string();
  }
  return std::string(kFileShellToolsContent);
}

std::string BuildTaskTrackingSection(bool has_todo) {
  if (!has_todo) {
    return std::string();
  }
  return std::string(kTaskTrackingContent);
}

std::string BuildSkillsSection(bool has_skill) {
  if (!has_skill) {
    return std::string();
  }
  return std::string(kSkillsContent);
}

std::string BuildParallelismTrailer() {
  // Always-on trailer — emitted alongside any other section. The caller
  // gates on `sections.empty()`.
  return std::string(kParallelismContent);
}

// Renders the umbrella "using-your-tools" section. Each child section's
// content is loaded verbatim from a .md file via the codegen-emitted
// constants; this function only handles the section header, gating,
// and join order.
std::string RenderUsingYourToolsSection(
    const std::vector<std::string>& enabled_tool_names) {
  std::set<std::string_view> enabled;
  for (const std::string& name : enabled_tool_names) {
    // string_view backed by the caller's vector — safe for the
    // duration of this call.
    enabled.insert(std::string_view(name));
  }

  std::vector<std::string> sections;

  std::string browser = BuildBrowserToolsSection(enabled);
  if (!browser.empty()) {
    sections.push_back(std::move(browser));
  }

  std::string file_shell = BuildFileShellSection(enabled);
  if (!file_shell.empty()) {
    sections.push_back(std::move(file_shell));
  }

  std::string task_tracking =
      BuildTaskTrackingSection(enabled.count(kTodoWriteName) != 0);
  if (!task_tracking.empty()) {
    sections.push_back(std::move(task_tracking));
  }

  std::string skills =
      BuildSkillsSection(enabled.count(kSkillName) != 0);
  if (!skills.empty()) {
    sections.push_back(std::move(skills));
  }

  if (sections.empty()) {
    return std::string();
  }
  sections.push_back(BuildParallelismTrailer());

  // Header + sections joined by a single blank line, matching the TS
  // ['# Using your tools', '', ...sections].join('\n') pattern.
  std::string out = "# Using your tools\n\n";
  for (size_t i = 0; i < sections.size(); ++i) {
    if (i > 0) {
      out.append("\n");
    }
    out.append(sections[i]);
  }
  return out;
}

}  // namespace

std::string RenderPromptSection(
    std::string_view name,
    const std::vector<std::string>& enabled_tool_names) {
  if (name == "using-your-tools") {
    return RenderUsingYourToolsSection(enabled_tool_names);
  }
  if (name == "system-prompt-tools") {
    // Phase 13.2: returns codegen-emitted prose verbatim. The .md at
    // `tools/declarations/system-prompt-tools.md` is the single source
    // of truth; `enabled_tool_names` is ignored because the prose is
    // cross-cutting (mentions many tools per bullet).
    return std::string(::nexus::generated::kSystemPromptToolsContent);
  }
  // Unknown section → empty string. Matches the TS behavior where an
  // unsupported `{{...}}` placeholder leaves the slot empty.
  return std::string();
}

std::vector<std::string> ListPromptSectionNames() {
  return {"using-your-tools", "system-prompt-tools"};
}

bool IsKnownPromptSection(std::string_view name) {
  for (const std::string& known : ListPromptSectionNames()) {
    if (known == name) {
      return true;
    }
  }
  return false;
}

}  // namespace nexus
