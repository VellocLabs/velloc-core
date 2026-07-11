#include "custom_browser/browser/nexus/api/catalog/prompt_holder_registry.h"

#include <ostream>
#include <vector>

#include "base/check.h"
#include "base/no_destructor.h"

namespace nexus {

namespace {

// Pack a (provider, locale) tuple into a stable key. The unit separator
// (0x1f) can't appear in any axis value.
std::string MakeKey(const std::string& provider, const std::string& locale) {
  return provider + "\x1f" + locale;
}

// Most-specific-first fallback chain, broadening to the unconditioned
// default last. Mirrors `fallbackChain` in the TS resolver double
// (__tests__/helpers/promptHolderResolver.ts) exactly.
std::vector<std::string> FallbackKeys(const HolderCondition& c) {
  const std::string& p = c.provider;
  const std::string& l = c.locale;
  std::vector<std::string> ordered = {
      MakeKey(p, l),
      MakeKey(p, ""),
      MakeKey("", l),
      MakeKey("", ""),
  };
  // De-dup while preserving order (empty axes collapse some keys).
  std::vector<std::string> deduped;
  for (const std::string& key : ordered) {
    bool seen = false;
    for (const std::string& kept : deduped) {
      if (kept == key) {
        seen = true;
        break;
      }
    }
    if (!seen) {
      deduped.push_back(key);
    }
  }
  return deduped;
}

}  // namespace

// static
PromptHolderRegistry& PromptHolderRegistry::Get() {
  static base::NoDestructor<PromptHolderRegistry> instance;
  return *instance;
}

PromptHolderRegistry::PromptHolderRegistry() = default;
PromptHolderRegistry::~PromptHolderRegistry() = default;

void PromptHolderRegistry::RegisterDefault(const std::string& holder,
                                           const std::string& content) {
  Register(HolderCondition(), holder, content);
}

void PromptHolderRegistry::Register(const HolderCondition& condition,
                                    const std::string& holder,
                                    const std::string& content) {
  if (holder.empty()) {
    return;
  }
  std::map<std::string, std::string>& by_key = overrides_[holder];
  const std::string key = MakeKey(condition.provider, condition.locale);
  // Each (holder, condition) is registered AT MOST ONCE. The boot publish
  // Clear()s first (HandleRegisterPromptOverridesOnUI), so a duplicate here
  // means the override batch listed the same holder+condition twice — the
  // second would silently overwrite the first. A holder name may still appear
  // multiple times under DIFFERENT conditions (e.g. an unconditioned default
  // plus a per-provider variant); only the same (holder, provider, locale) is
  // a bug.
  DCHECK(by_key.find(key) == by_key.end())
      << "prompt holder '" << holder << "' registered twice for (provider='"
      << condition.provider << "', locale='" << condition.locale << "')";
  by_key[key] = content;
}

void PromptHolderRegistry::Clear() {
  overrides_.clear();
}

std::optional<std::string> PromptHolderRegistry::ResolveOverride(
    const std::string& holder,
    const HolderCondition& condition) const {
  const auto holder_it = overrides_.find(holder);
  if (holder_it == overrides_.end()) {
    return std::nullopt;
  }
  const std::map<std::string, std::string>& by_key = holder_it->second;
  for (const std::string& key : FallbackKeys(condition)) {
    const auto it = by_key.find(key);
    if (it != by_key.end()) {
      return it->second;
    }
  }
  return std::nullopt;
}

void PromptHolderRegistry::RegisterToolDef(const std::string& name,
                                           base::DictValue def) {
  if (name.empty()) {
    return;
  }
  tool_defs_[name] = std::move(def);
}

const base::DictValue* PromptHolderRegistry::ResolveToolDef(
    const std::string& name) const {
  const auto it = tool_defs_.find(name);
  return it != tool_defs_.end() ? &it->second : nullptr;
}

}  // namespace nexus
