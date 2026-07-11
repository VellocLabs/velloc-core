// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/tools/tool_session.h"

#include "base/time/time.h"
#include "custom_browser/browser/nexus/tools/browser_tool.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

// `ToolSession`'s ctor takes `Profile*`, but only the per-category
// constructors (`BrowserTool`) consume it, and they don't dereference
// in their ctor — they just store the pointer through `ToolBase`. So
// passing `nullptr` is safe for these pure-state tests, the same way
// `tool_base_unittest.cc` does. We never run a real tool here; we're
// only exercising the timeout-knob plumbing.
constexpr char kSessionKey[] = "test-session";

TEST(ToolSessionTest, ActionTimeoutDefaultsTo180Seconds) {
  ToolSession session(/*profile=*/nullptr, kSessionKey);
  EXPECT_EQ(session.action_timeout(), base::Seconds(180));
  // The default must also have been pushed down to the BrowserTool
  // member so per-tool BudgetWrap calls see the same value without
  // anyone calling `set_action_timeout` first.
  EXPECT_EQ(session.browser_tool().action_timeout(), base::Seconds(180));
}

TEST(ToolSessionTest, ActionTimeoutAcceptsValueInRange) {
  ToolSession session(/*profile=*/nullptr, kSessionKey);
  session.set_action_timeout(base::Seconds(45));
  EXPECT_EQ(session.action_timeout(), base::Seconds(45));
  EXPECT_EQ(session.browser_tool().action_timeout(), base::Seconds(45));
}

TEST(ToolSessionTest, ActionTimeoutClampsBelowOneSecond) {
  ToolSession session(/*profile=*/nullptr, kSessionKey);
  // Explicit zero — a misconfigured caller must NOT be able to disable
  // the budget. The `1s..600s` range is the contract documented on
  // `set_action_timeout` and matches the JS-side validation.
  session.set_action_timeout(base::TimeDelta());
  EXPECT_EQ(session.action_timeout(), base::Seconds(1));
  EXPECT_EQ(session.browser_tool().action_timeout(), base::Seconds(1));
}

TEST(ToolSessionTest, ActionTimeoutClampsNegativeValuesUp) {
  ToolSession session(/*profile=*/nullptr, kSessionKey);
  session.set_action_timeout(base::Milliseconds(-50));
  EXPECT_EQ(session.action_timeout(), base::Seconds(1));
  EXPECT_EQ(session.browser_tool().action_timeout(), base::Seconds(1));
}

TEST(ToolSessionTest, ActionTimeoutClampsSubSecondValuesUp) {
  ToolSession session(/*profile=*/nullptr, kSessionKey);
  // 999ms is below the floor — clamp to 1s.
  session.set_action_timeout(base::Milliseconds(999));
  EXPECT_EQ(session.action_timeout(), base::Seconds(1));
  EXPECT_EQ(session.browser_tool().action_timeout(), base::Seconds(1));
}

TEST(ToolSessionTest, ActionTimeoutAllowsExactBoundaryOneSecond) {
  ToolSession session(/*profile=*/nullptr, kSessionKey);
  session.set_action_timeout(base::Seconds(1));
  EXPECT_EQ(session.action_timeout(), base::Seconds(1));
  EXPECT_EQ(session.browser_tool().action_timeout(), base::Seconds(1));
}

TEST(ToolSessionTest, ActionTimeoutAllowsExactBoundarySixHundredSeconds) {
  ToolSession session(/*profile=*/nullptr, kSessionKey);
  session.set_action_timeout(base::Seconds(600));
  EXPECT_EQ(session.action_timeout(), base::Seconds(600));
  EXPECT_EQ(session.browser_tool().action_timeout(), base::Seconds(600));
}

TEST(ToolSessionTest, ActionTimeoutClampsAboveTenMinutes) {
  ToolSession session(/*profile=*/nullptr, kSessionKey);
  // 1 hour is well above the ceiling.
  session.set_action_timeout(base::Minutes(60));
  EXPECT_EQ(session.action_timeout(), base::Seconds(600));
  EXPECT_EQ(session.browser_tool().action_timeout(), base::Seconds(600));
}

TEST(ToolSessionTest, ActionTimeoutPropagatesOnSubsequentSets) {
  ToolSession session(/*profile=*/nullptr, kSessionKey);
  session.set_action_timeout(base::Seconds(30));
  ASSERT_EQ(session.browser_tool().action_timeout(), base::Seconds(30));

  // Production paths set once at session start, but if a caller does
  // re-set, every set must propagate (the BrowserTool member doesn't
  // hold a back-pointer — propagation is push-only).
  session.set_action_timeout(base::Seconds(120));
  EXPECT_EQ(session.action_timeout(), base::Seconds(120));
  EXPECT_EQ(session.browser_tool().action_timeout(), base::Seconds(120));
}

TEST(ToolSessionTest, StepTimeoutDefaultsTo180Seconds) {
  ToolSession session(/*profile=*/nullptr, kSessionKey);
  EXPECT_EQ(session.step_timeout(), base::Seconds(180));
}

TEST(ToolSessionTest, StepTimeoutClampsBelowOneSecond) {
  ToolSession session(/*profile=*/nullptr, kSessionKey);
  session.set_step_timeout(base::TimeDelta());
  EXPECT_EQ(session.step_timeout(), base::Seconds(1));
}

TEST(ToolSessionTest, StepTimeoutClampsAboveTenMinutes) {
  ToolSession session(/*profile=*/nullptr, kSessionKey);
  session.set_step_timeout(base::Hours(1));
  EXPECT_EQ(session.step_timeout(), base::Seconds(600));
}

TEST(ToolSessionTest, StepTimeoutAcceptsValueInRange) {
  ToolSession session(/*profile=*/nullptr, kSessionKey);
  session.set_step_timeout(base::Seconds(90));
  EXPECT_EQ(session.step_timeout(), base::Seconds(90));
}

TEST(ToolSessionTest, ActionAndStepTimeoutsAreIndependent) {
  ToolSession session(/*profile=*/nullptr, kSessionKey);
  session.set_action_timeout(base::Seconds(30));
  session.set_step_timeout(base::Seconds(120));

  EXPECT_EQ(session.action_timeout(), base::Seconds(30));
  EXPECT_EQ(session.step_timeout(), base::Seconds(120));
  // Setting action_timeout must not perturb step_timeout, and vice versa.
  session.set_action_timeout(base::Seconds(60));
  EXPECT_EQ(session.step_timeout(), base::Seconds(120));
}

TEST(ToolSessionTest, SessionKeyIsExposedAsConstructed) {
  ToolSession session(/*profile=*/nullptr, "keyed-session-A");
  EXPECT_EQ(session.session_key(), "keyed-session-A");
  // Default profile pointer round-trips through.
  EXPECT_EQ(session.profile(), nullptr);
}

TEST(ToolSessionTest, OnStopIsIdempotent) {
  ToolSession session(/*profile=*/nullptr, kSessionKey);
  // Cascading OnStop multiple times must be safe — the dtor also runs
  // OnStop("destructor"), so any explicit call must tolerate being
  // followed by a second one without crashing or leaking.
  session.OnStop("first");
  session.OnStop("second");
  // No assert here other than the implicit "no crash" — the dtor runs
  // on scope exit and exercises the third call.
}

}  // namespace
}  // namespace nexus
