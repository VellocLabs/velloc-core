#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_CATALOG_PROMPT_HOLDER_REGISTRY_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_CATALOG_PROMPT_HOLDER_REGISTRY_H_

#include <map>
#include <optional>
#include <string>

#include "base/no_destructor.h"
#include "base/values.h"

namespace nexus {

// Resolution condition for a prompt-holder override. Empty axes are
// wildcards. v1 ships provider/locale — the browser LLM module fills these
// from the request's provider id and the request's system-context locale.
struct HolderCondition {
  std::string provider;
  std::string locale;
};

// Process-global registry of conditional overrides for system-prompt
// section holders. The browser LLM module resolves a `{ h }` holder by
// first consulting this registry (most-specific (provider, locale) match,
// falling back to broader conditions, then the unconditioned default) and
// only falling back to the built-in renderer (`RenderPromptSection`) / the
// holder's carried default when nothing is registered. This is the
// central-management seam: nexus_web publishes overrides once at boot via
// the `catalog.registerPromptOverrides` bridge action so prompt prose can
// be varied per provider/locale without changing the guest's per-turn
// payload. Singleton, UI-sequence affine — mirrors `ToolCatalog::Get()`.
//
// SCOPE: process-global, NOT session-scoped. Overrides and tool defs are
// keyed by (condition, holder) / name only — concurrent sessions in one
// process share them; a later publish overwrites an earlier one. The TS
// resolver double (__tests__/helpers/promptHolderResolver.ts) models this
// same flat scope.
class PromptHolderRegistry {
 public:
  static PromptHolderRegistry& Get();

  // Production code uses the process-global `Get()`. The ctor/dtor are public
  // only so unit tests can construct isolated, throwaway instances (no shared
  // global state between tests) — see prompt_holder_registry_unittest.cc.
  PromptHolderRegistry();
  ~PromptHolderRegistry();

  PromptHolderRegistry(const PromptHolderRegistry&) = delete;
  PromptHolderRegistry& operator=(const PromptHolderRegistry&) = delete;

  // Register an unconditioned default for `holder`.
  void RegisterDefault(const std::string& holder, const std::string& content);

  // Register a conditioned override for `holder`. Any empty axis is a
  // wildcard; all-empty is equivalent to RegisterDefault.
  void Register(const HolderCondition& condition,
                const std::string& holder,
                const std::string& content);

  // Drop all section-prose overrides (a fresh boot-time
  // `registerPromptOverrides` publish replaces them wholesale). Does NOT
  // touch registered tool defs — those have their own overwrite-by-name
  // lifecycle (see RegisterToolDef).
  void Clear();

  // Resolve `holder` for `condition`: most-specific match first, broadening
  // to the unconditioned default; nullopt when nothing matches (caller falls
  // back to the built-in renderer / carried default).
  std::optional<std::string> ResolveOverride(
      const std::string& holder,
      const HolderCondition& condition) const;

  // ── runtime tool definitions (refs for JS-only + MCP tools) ──────────
  // Register/overwrite a tool's full wire definition by name. nexus_web
  // publishes these for tools whose definition is NOT in the C++
  // ToolCatalog (static-JS at session init, MCP on connect) so they can
  // ship as `{ ref }` and the host fills the schema. Overwrite-by-name, so
  // a reconnecting MCP server simply replaces its prior def.
  //
  // NOTE: there is no removal path. A disconnected MCP server's def lingers
  // until overwritten or process exit; this is harmless because the guest
  // stops emitting that tool's `{ ref }` once the server drops (an orphaned
  // def is simply never resolved). Add an Unregister if a def ever needs to
  // be actively reclaimed.
  void RegisterToolDef(const std::string& name, base::DictValue def);

  // Resolve a tool ref to its registered definition dict, or nullptr.
  const base::DictValue* ResolveToolDef(const std::string& name) const;

 private:
  friend class base::NoDestructor<PromptHolderRegistry>;

  // holder -> conditionKey -> content
  std::map<std::string, std::map<std::string, std::string>> overrides_;
  // tool wire name -> full definition dict
  std::map<std::string, base::DictValue> tool_defs_;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_CATALOG_PROMPT_HOLDER_REGISTRY_H_
