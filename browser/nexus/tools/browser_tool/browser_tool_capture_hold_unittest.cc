// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include <memory>
#include <vector>

#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "chrome/test/base/testing_profile.h"
#include "components/tabs/public/mock_tab_interface.h"
#include "content/public/browser/web_contents.h"
#include "custom_browser/browser/nexus/nexus_test_support.h"
#include "custom_browser/browser/nexus/tools/browser_tool/browser_tool.h"
#include "custom_browser/browser/nexus/tools/tool_base.h"
#include "custom_browser/browser/nexus/tools/tool_session.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

using ::testing::Return;

// Agent tabs are held visible only for the turn: the hold forces compositing
// while tools run, and dropping it at OnStop lets a hidden agent tab release
// its render caches between turns (F-21).
class BrowserToolCaptureHoldTest : public ChromeRenderViewHostTestHarness {
 protected:
  // An agent tab: a real WebContents behind a mock TabInterface, so its
  // handle resolves without a Browser.
  struct AgentTab {
    std::unique_ptr<content::WebContents> contents;
    std::unique_ptr<tabs::MockTabInterface> tab;
  };

  TestingProfile::TestingFactories GetTestingFactories() const override {
    return test::GlicDisablingTestingFactories();
  }

  void SetUp() override {
    ChromeRenderViewHostTestHarness::SetUp();
    session_ = std::make_unique<ToolSession>(profile(), "session-A");
  }

  void TearDown() override {
    session_.reset();
    tabs_.clear();
    ChromeRenderViewHostTestHarness::TearDown();
  }

  content::WebContents* AddTab() {
    AgentTab agent_tab;
    agent_tab.contents = CreateTestWebContents();
    agent_tab.tab = std::make_unique<tabs::MockTabInterface>();
    ON_CALL(*agent_tab.tab, GetContents())
        .WillByDefault(Return(agent_tab.contents.get()));
    tabs_.push_back(std::move(agent_tab));
    return tabs_.back().contents.get();
  }

  std::vector<tabs::TabHandle> Handles() const {
    std::vector<tabs::TabHandle> handles;
    for (const AgentTab& agent_tab : tabs_) {
      handles.push_back(agent_tab.tab->GetHandle());
    }
    return handles;
  }

  BrowserTool& tool() { return session_->browser_tool(); }

  std::vector<AgentTab> tabs_;
  std::unique_ptr<ToolSession> session_;
};

TEST_F(BrowserToolCaptureHoldTest, HoldKeepsTheTabVisiblyCaptured) {
  content::WebContents* contents = AddTab();
  ASSERT_FALSE(contents->IsBeingCaptured());

  tool().HoldTabsForTurn(Handles());

  EXPECT_TRUE(contents->IsBeingVisiblyCaptured());
}

TEST_F(BrowserToolCaptureHoldTest, TurnCompletionReleasesTheHold) {
  content::WebContents* contents = AddTab();

  tool().HoldTabsForTurn(Handles());
  ASSERT_TRUE(contents->IsBeingCaptured());

  tool().OnStop(OnStopReason::kCompleted);
  EXPECT_FALSE(contents->IsBeingCaptured());
}

TEST_F(BrowserToolCaptureHoldTest, UserStopReleasesTheHold) {
  content::WebContents* contents = AddTab();

  tool().HoldTabsForTurn(Handles());
  tool().OnStop(OnStopReason::kUserStopped);

  EXPECT_FALSE(contents->IsBeingCaptured());
}

TEST_F(BrowserToolCaptureHoldTest, RepeatedDispatchesTakeOneHoldPerTab) {
  content::WebContents* contents = AddTab();

  tool().HoldTabsForTurn(Handles());
  tool().HoldTabsForTurn(Handles());
  tool().HoldTabsForTurn(Handles());
  tool().OnStop(OnStopReason::kCompleted);

  // One release balances every dispatch of the turn.
  EXPECT_FALSE(contents->IsBeingCaptured());
}

TEST_F(BrowserToolCaptureHoldTest, NextTurnTakesTheHoldAgain) {
  content::WebContents* contents = AddTab();

  tool().HoldTabsForTurn(Handles());
  tool().OnStop(OnStopReason::kCompleted);
  ASSERT_FALSE(contents->IsBeingCaptured());

  tool().HoldTabsForTurn(Handles());
  EXPECT_TRUE(contents->IsBeingVisiblyCaptured());
}

TEST_F(BrowserToolCaptureHoldTest, TabAddedMidTurnJoinsTheTurnsHolds) {
  content::WebContents* first = AddTab();
  tool().HoldTabsForTurn(Handles());

  content::WebContents* second = AddTab();
  tool().HoldTabsForTurn(Handles());
  EXPECT_TRUE(first->IsBeingCaptured());
  EXPECT_TRUE(second->IsBeingCaptured());

  tool().OnStop(OnStopReason::kCompleted);
  EXPECT_FALSE(first->IsBeingCaptured());
  EXPECT_FALSE(second->IsBeingCaptured());
}

TEST_F(BrowserToolCaptureHoldTest, ClosedTabIsSkippedAndItsHoldIsHarmless) {
  content::WebContents* survivor = AddTab();
  AddTab();
  tool().HoldTabsForTurn(Handles());

  // The second tab closes mid-turn; the next dispatch and the release must
  // not touch it.
  tabs_.pop_back();
  tool().HoldTabsForTurn(Handles());
  tool().OnStop(OnStopReason::kCompleted);

  EXPECT_FALSE(survivor->IsBeingCaptured());
}

}  // namespace
}  // namespace nexus
