// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Native golden for the prompt-holder registry. Mirrors the TS resolver
// double (nexus_web/src/__tests__/helpers/promptHolderResolver.ts +
// promptHolderResolution.test.ts) so both sides assert the SAME contract:
// the (provider, locale) most-specific-first fallback chain, the
// unconditioned default, "no match → nullopt" (caller falls back to the
// built-in renderer / carried default), process-global tool-def
// resolution/overwrite, and Clear()'s overrides-only scope.

#include "custom_browser/browser/nexus/api/catalog/prompt_holder_registry.h"

#include <string>

#include "base/test/gtest_util.h"
#include "base/values.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

// A tool-def dict carrying a single marker field so tests can tell which
// registration won.
base::DictValue ToolDef(const std::string& marker) {
  base::DictValue def;
  def.Set("name", "SomeTool");
  def.Set("marker", marker);
  return def;
}

TEST(PromptHolderRegistryTest, ResolvesUnconditionedDefault) {
  PromptHolderRegistry reg;
  reg.RegisterDefault("intro", "DEFAULT INTRO");
  EXPECT_EQ(reg.ResolveOverride("intro", HolderCondition()), "DEFAULT INTRO");
}

TEST(PromptHolderRegistryTest, EmptyConditionEqualsRegisterDefault) {
  PromptHolderRegistry reg;
  reg.Register(HolderCondition(), "tone", "VIA EMPTY CONDITION");
  EXPECT_EQ(reg.ResolveOverride("tone", HolderCondition()), "VIA EMPTY CONDITION");
}

TEST(PromptHolderRegistryTest, UnregisteredHolderResolvesToNullopt) {
  PromptHolderRegistry reg;
  // Nothing registered for "system" at all → nullopt (caller falls back to
  // RenderPromptSection / the carried default).
  EXPECT_FALSE(reg.ResolveOverride("system", HolderCondition()).has_value());
  // A holder with only an unconditioned default still matches any condition
  // via the ("","") tail of the chain.
  reg.RegisterDefault("intro", "X");
  EXPECT_TRUE(reg.ResolveOverride("intro", {"openai", "en"}).has_value());
}

TEST(PromptHolderRegistryTest, ProviderLocalePrecedenceChain) {
  PromptHolderRegistry reg;
  const std::string kHolder = "tone";
  reg.RegisterDefault(kHolder, "DEFAULT");
  reg.Register({/*provider=*/"openai", /*locale=*/""}, kHolder, "OPENAI");
  reg.Register({/*provider=*/"", /*locale=*/"en"}, kHolder, "EN");
  reg.Register({/*provider=*/"openai", /*locale=*/"en"}, kHolder, "EXACT");

  // Most-specific (provider, locale) pair wins.
  EXPECT_EQ(reg.ResolveOverride(kHolder, {"openai", "en"}), "EXACT");
  // provider beats locale in the chain: (openai, fr) → (openai, "").
  EXPECT_EQ(reg.ResolveOverride(kHolder, {"openai", "fr"}), "OPENAI");
  // locale-only match when the provider is unknown: (google, en) → ("", en).
  EXPECT_EQ(reg.ResolveOverride(kHolder, {"google", "en"}), "EN");
  // nothing matches → the unconditioned default.
  EXPECT_EQ(reg.ResolveOverride(kHolder, {"google", "fr"}), "DEFAULT");
}

TEST(PromptHolderRegistryTest, ProviderBeatsLocaleWithoutDefault) {
  PromptHolderRegistry reg;
  const std::string kHolder = "system";
  reg.Register({"openai", ""}, kHolder, "OPENAI");
  reg.Register({"", "en"}, kHolder, "EN");
  // (openai, en): (openai,en) miss → (openai,"") HIT → provider wins.
  EXPECT_EQ(reg.ResolveOverride(kHolder, {"openai", "en"}), "OPENAI");
  // (google, en): (google,en)/(google,"") miss → ("", en) HIT.
  EXPECT_EQ(reg.ResolveOverride(kHolder, {"google", "en"}), "EN");
  // (google, fr): no default registered → nullopt.
  EXPECT_FALSE(reg.ResolveOverride(kHolder, {"google", "fr"}).has_value());
}

TEST(PromptHolderRegistryTest, EmptyHolderNameIsIgnored) {
  PromptHolderRegistry reg;
  reg.RegisterDefault("", "NOPE");
  reg.Register({"openai", ""}, "", "NOPE2");
  EXPECT_FALSE(reg.ResolveOverride("", HolderCondition()).has_value());
}

TEST(PromptHolderRegistryTest, DuplicateSameConditionRegistrationTripsDCheck) {
  PromptHolderRegistry reg;
  reg.RegisterDefault("intro", "FIRST");
  // Re-registering the SAME (holder, condition) would silently overwrite the
  // first — a config bug (e.g. a registerPromptOverrides batch listing the
  // same holder+condition twice). Guarded by a register-once DCHECK.
  EXPECT_DCHECK_DEATH(reg.RegisterDefault("intro", "SECOND"));
}

TEST(PromptHolderRegistryTest, SameHolderDifferentConditionsDoNotTrip) {
  PromptHolderRegistry reg;
  // A holder legitimately appears under multiple DISTINCT conditions (an
  // unconditioned default + per-provider/locale variants); that is not a
  // duplicate and must not trip the register-once DCHECK.
  reg.RegisterDefault("intro", "DEFAULT");
  reg.Register({"openai", ""}, "intro", "OPENAI");
  reg.Register({"", "en"}, "intro", "EN");
  EXPECT_EQ(reg.ResolveOverride("intro", {"openai", "en"}), "OPENAI");
}

TEST(PromptHolderRegistryTest, ToolDefRoundTripsByName) {
  PromptHolderRegistry reg;
  reg.RegisterToolDef("BrowserClick", ToolDef("v1"));

  const base::DictValue* def = reg.ResolveToolDef("BrowserClick");
  ASSERT_NE(def, nullptr);
  const std::string* marker = def->FindString("marker");
  ASSERT_NE(marker, nullptr);
  EXPECT_EQ(*marker, "v1");

  EXPECT_EQ(reg.ResolveToolDef("does_not_exist"), nullptr);
}

TEST(PromptHolderRegistryTest, ToolDefOverwritesByName) {
  PromptHolderRegistry reg;
  reg.RegisterToolDef("github_create_issue", ToolDef("stale"));
  // e.g. an MCP server reconnecting re-publishes its def under the same name.
  reg.RegisterToolDef("github_create_issue", ToolDef("fresh"));

  const base::DictValue* def = reg.ResolveToolDef("github_create_issue");
  ASSERT_NE(def, nullptr);
  const std::string* marker = def->FindString("marker");
  ASSERT_NE(marker, nullptr);
  EXPECT_EQ(*marker, "fresh");
}

TEST(PromptHolderRegistryTest, EmptyToolNameIsIgnored) {
  PromptHolderRegistry reg;
  reg.RegisterToolDef("", ToolDef("nope"));
  EXPECT_EQ(reg.ResolveToolDef(""), nullptr);
}

TEST(PromptHolderRegistryTest, ClearDropsOverridesButNotToolDefs) {
  PromptHolderRegistry reg;
  reg.RegisterDefault("intro", "INTRO");
  reg.RegisterToolDef("BrowserClick", ToolDef("v1"));

  reg.Clear();

  // Section overrides are dropped (a fresh boot-time publish replaces them).
  EXPECT_FALSE(reg.ResolveOverride("intro", HolderCondition()).has_value());
  // Tool defs have their own overwrite-by-name lifecycle and survive Clear().
  EXPECT_NE(reg.ResolveToolDef("BrowserClick"), nullptr);
}

}  // namespace
}  // namespace nexus
