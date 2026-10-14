// Copyright 2026 VellocLabs
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/tools/browser_tool/browser_tool_internal.h"

#include <string>

#include "base/values.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus::browser_tool_internal {
namespace {

// ── CompressHref ──────────────────────────────────────────────────────────

TEST(CompressHrefTest, SameOriginDropsSchemeAndHost) {
  bool truncated = true;
  EXPECT_EQ(CompressHref("https://example.com/foo/bar?x=1",
                         "https://example.com/home", &truncated),
            "/foo/bar?x=1");
  EXPECT_FALSE(truncated);
}

TEST(CompressHrefTest, SameOriginRequiresMatchingPort) {
  // Different port ⇒ not same-origin ⇒ keep host.
  EXPECT_EQ(CompressHref("https://example.com:8443/foo",
                         "https://example.com/home", nullptr),
            "https://example.com:8443/foo");
}

TEST(CompressHrefTest, CrossOriginKeepsSchemeHostPath) {
  EXPECT_EQ(CompressHref("https://other.com/a/b",
                         "https://example.com/home", nullptr),
            "https://other.com/a/b");
}

TEST(CompressHrefTest, StripsTrackingParams) {
  // utm_* prefix + known keys dropped; genuine params survive, order kept.
  EXPECT_EQ(
      CompressHref(
          "https://example.com/p?utm_source=x&id=7&fbclid=abc&gclid=z&q=hi",
          "https://example.com/home", nullptr),
      "/p?id=7&q=hi");
}

TEST(CompressHrefTest, AllTrackingParamsLeavesNoQuery) {
  EXPECT_EQ(CompressHref("https://example.com/p?utm_source=x&fbclid=y",
                         "https://example.com/home", nullptr),
            "/p");
}

TEST(CompressHrefTest, PreservesFragment) {
  EXPECT_EQ(CompressHref("https://example.com/p?a=1#sec",
                         "https://example.com/home", nullptr),
            "/p?a=1#sec");
}

TEST(CompressHrefTest, NonHttpPassesThrough) {
  EXPECT_EQ(CompressHref("mailto:me@example.com", "https://example.com/home",
                         nullptr),
            "mailto:me@example.com");
  EXPECT_EQ(CompressHref("javascript:void(0)", "https://example.com/home",
                         nullptr),
            "javascript:void(0)");
}

TEST(CompressHrefTest, EmptyPageUrlSkipsSameOriginButStillStrips) {
  EXPECT_EQ(CompressHref("https://example.com/p?utm_source=x&a=1", "", nullptr),
            "https://example.com/p?a=1");
}

TEST(CompressHrefTest, TruncatesOverCap) {
  std::string long_path = "/";
  long_path.append(kHrefMaxBytes + 50, 'a');
  bool truncated = false;
  const std::string out = CompressHref(
      "https://example.com" + long_path, "https://example.com/home",
      &truncated);
  EXPECT_TRUE(truncated);
  EXPECT_LE(static_cast<int>(out.size()), kHrefMaxBytes);
}

// ── ShouldShortCircuitObservation ─────────────────────────────────────────

TEST(ShouldShortCircuitTest, EqualHashSameDocEnabledFreshShortCircuits) {
  EXPECT_TRUE(ShouldShortCircuitObservation(
      /*enabled=*/true, /*stale=*/false, "doc1", "doc1", "hashA", "hashA"));
}

TEST(ShouldShortCircuitTest, DisabledNeverShortCircuits) {
  EXPECT_FALSE(ShouldShortCircuitObservation(false, false, "doc1", "doc1",
                                             "hashA", "hashA"));
}

TEST(ShouldShortCircuitTest, StaleForcesFull) {
  EXPECT_FALSE(ShouldShortCircuitObservation(true, /*stale=*/true, "doc1",
                                             "doc1", "hashA", "hashA"));
}

TEST(ShouldShortCircuitTest, DocumentChangeForcesFull) {
  EXPECT_FALSE(ShouldShortCircuitObservation(true, false, "doc1", "doc2",
                                             "hashA", "hashA"));
}

TEST(ShouldShortCircuitTest, HashChangeForcesFull) {
  EXPECT_FALSE(ShouldShortCircuitObservation(true, false, "doc1", "doc1",
                                             "hashA", "hashB"));
}

TEST(ShouldShortCircuitTest, EmptyPriorStateForcesFull) {
  // First observation: no prior hash/doc recorded yet.
  EXPECT_FALSE(ShouldShortCircuitObservation(true, false, "", "doc1", "",
                                             "hashA"));
}

// ── ObservationBaseline ───────────────────────────────────────────────────
// The baseline must move ONLY when the elements[] ship to the model. These
// pin the stage → commit / discard contract that BrowserTool's resolve sites
// rely on (review finding: a stripped read_page moved the baseline to a
// surface the model never received, so the next same-document action
// collapsed to an {unchanged} stub while the model still held the old page).

TEST(ObservationBaselineTest, FirstObservationNeverShortCircuits) {
  ObservationBaseline b;
  EXPECT_FALSE(b.ShouldShortCircuit(true, "doc1", "hashA"));
  EXPECT_FALSE(b.has_staged());
}

TEST(ObservationBaselineTest, StagedIsInvisibleUntilCommit) {
  ObservationBaseline b;
  b.Stage("doc1", "hashA");
  EXPECT_TRUE(b.has_staged());
  // Staged only — a repeat of the same surface is still a full send.
  EXPECT_FALSE(b.ShouldShortCircuit(true, "doc1", "hashA"));
  EXPECT_TRUE(b.Commit());
  EXPECT_FALSE(b.has_staged());
  EXPECT_TRUE(b.ShouldShortCircuit(true, "doc1", "hashA"));
  EXPECT_EQ(b.document_id(), "doc1");
  EXPECT_EQ(b.projection(), "hashA");
}

TEST(ObservationBaselineTest, DiscardKeepsThePriorBaseline) {
  // The finding-2 scenario: navigate (model sees P1) → page mutates →
  // default read_page walks P2 but strips it → the baseline must stay P1
  // so the next action on the (still P2) surface is sent full.
  ObservationBaseline b;
  b.Stage("doc1", "P1");
  ASSERT_TRUE(b.Commit());
  b.Stage("doc1", "P2");
  b.Discard();
  EXPECT_FALSE(b.has_staged());
  EXPECT_FALSE(b.ShouldShortCircuit(true, "doc1", "P2"));
  EXPECT_TRUE(b.ShouldShortCircuit(true, "doc1", "P1"));
  EXPECT_EQ(b.projection(), "P1");
}

TEST(ObservationBaselineTest, CommitReplacesTheBaseline) {
  ObservationBaseline b;
  b.Stage("doc1", "P1");
  ASSERT_TRUE(b.Commit());
  b.Stage("doc1", "P2");
  ASSERT_TRUE(b.Commit());
  EXPECT_TRUE(b.ShouldShortCircuit(true, "doc1", "P2"));
  EXPECT_FALSE(b.ShouldShortCircuit(true, "doc1", "P1"));
}

TEST(ObservationBaselineTest, RestageReplacesTheStagedValue) {
  // A walk whose result was never resolved (settle guard re-observe) is
  // superseded by the next walk; committing yields the LATEST projection.
  ObservationBaseline b;
  b.Stage("doc1", "P1");
  b.Stage("doc1", "P2");
  ASSERT_TRUE(b.Commit());
  EXPECT_TRUE(b.ShouldShortCircuit(true, "doc1", "P2"));
  EXPECT_FALSE(b.ShouldShortCircuit(true, "doc1", "P1"));
}

TEST(ObservationBaselineTest, CommitWithoutStageIsANoOp) {
  // A short-circuit stub stages nothing; the caller's unconditional
  // Commit() must leave the baseline exactly as it was.
  ObservationBaseline b;
  b.Stage("doc1", "P1");
  ASSERT_TRUE(b.Commit());
  EXPECT_FALSE(b.Commit());
  EXPECT_TRUE(b.ShouldShortCircuit(true, "doc1", "P1"));
}

TEST(ObservationBaselineTest, MarkStaleForcesFullUntilTheNextCommit) {
  ObservationBaseline b;
  b.Stage("doc1", "P1");
  ASSERT_TRUE(b.Commit());
  b.MarkStale();
  EXPECT_TRUE(b.stale());
  EXPECT_FALSE(b.ShouldShortCircuit(true, "doc1", "P1"));
  // The next full send re-establishes the baseline and clears staleness.
  b.Stage("doc1", "P1");
  ASSERT_TRUE(b.Commit());
  EXPECT_FALSE(b.stale());
  EXPECT_TRUE(b.ShouldShortCircuit(true, "doc1", "P1"));
}

TEST(ObservationBaselineTest, MarkStaleDropsAnythingStaged) {
  // A context mutation that lands between a walk and its resolve: the
  // conservative outcome is "no baseline" — the next observation is full.
  ObservationBaseline b;
  b.Stage("doc1", "P1");
  ASSERT_TRUE(b.Commit());
  b.Stage("doc1", "P2");
  b.MarkStale();
  EXPECT_FALSE(b.has_staged());
  EXPECT_FALSE(b.Commit());
  EXPECT_TRUE(b.stale());
  EXPECT_FALSE(b.ShouldShortCircuit(true, "doc1", "P2"));
  EXPECT_FALSE(b.ShouldShortCircuit(true, "doc1", "P1"));
}

TEST(ObservationBaselineTest, DisabledNeverShortCircuits) {
  ObservationBaseline b;
  b.Stage("doc1", "P1");
  ASSERT_TRUE(b.Commit());
  EXPECT_FALSE(b.ShouldShortCircuit(/*enabled=*/false, "doc1", "P1"));
}

TEST(ObservationBaselineTest, DocumentChangeForcesFull) {
  ObservationBaseline b;
  b.Stage("doc1", "P1");
  ASSERT_TRUE(b.Commit());
  EXPECT_FALSE(b.ShouldShortCircuit(true, "doc2", "P1"));
}

// ── StripObservationToBasics ──────────────────────────────────────────────

base::DictValue MakeObservation() {
  base::DictValue obs;
  base::ListValue tabs;

  base::DictValue tab0;
  tab0.Set("tab_id", 11);
  tab0.Set("url", "https://example.com/");
  tab0.Set("title", "Home");
  base::ListValue els;
  base::DictValue e;
  e.Set("index", 42);
  e.Set("type", "link");
  els.Append(std::move(e));
  tab0.Set("elements", std::move(els));
  tabs.Append(std::move(tab0));

  base::DictValue tab1;
  tab1.Set("tab_id", 12);
  tab1.Set("url", "https://example.com/other");
  tab1.Set("title", "Other");  // basics-only already (no elements)
  tabs.Append(std::move(tab1));

  obs.Set("tabs", std::move(tabs));
  return obs;
}

TEST(StripObservationToBasicsTest, RemovesElementsKeepsBasics) {
  base::DictValue obs = MakeObservation();
  const int stripped = StripObservationToBasics(obs);
  EXPECT_EQ(stripped, 1);  // only tab0 had elements

  const base::ListValue* tabs = obs.FindList("tabs");
  ASSERT_TRUE(tabs);
  ASSERT_EQ(tabs->size(), 2u);

  const base::DictValue& t0 = (*tabs)[0].GetDict();
  EXPECT_FALSE(t0.contains("elements"));
  EXPECT_EQ(*t0.FindInt("tab_id"), 11);
  EXPECT_EQ(*t0.FindString("url"), "https://example.com/");
  EXPECT_EQ(*t0.FindString("title"), "Home");

  const base::DictValue& t1 = (*tabs)[1].GetDict();
  EXPECT_FALSE(t1.contains("elements"));
  EXPECT_EQ(*t1.FindInt("tab_id"), 12);
}

TEST(StripObservationToBasicsTest, NoTabsIsNoOp) {
  base::DictValue obs;
  EXPECT_EQ(StripObservationToBasics(obs), 0);
}

}  // namespace
}  // namespace nexus::browser_tool_internal
