// Copyright 2026 The Custom Browser Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/panel/nexus_background_panel_host.h"

#include <memory>

#include "chrome/browser/profiles/profile.h"
#include "chrome/common/pref_names.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "chrome/test/base/testing_browser_process.h"
#include "chrome/test/base/testing_profile.h"
#include "components/prefs/testing_pref_service.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/test/mock_render_process_host.h"
#include "content/public/test/web_contents_tester.h"
#include "custom_browser/browser/nexus/nexus_test_support.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

class NexusBackgroundPanelHostTest : public ChromeRenderViewHostTestHarness {
 protected:
  TestingProfile::TestingFactories GetTestingFactories() const override {
    return test::GlicDisablingTestingFactories();
  }

  TestingPrefServiceSimple* local_state() {
    return TestingBrowserProcess::GetGlobal()->GetTestingLocalState();
  }

  NexusBackgroundPanelHost* host() {
    return NexusBackgroundPanelHostFactory::GetForProfile(profile());
  }

  std::unique_ptr<content::WebContents> MakeContents() {
    return content::WebContentsTester::CreateTestWebContents(profile(),
                                                             nullptr);
  }
};

// The System toggle is Chromium's own local_state pref, registered true by
// default — "background apps keep running" is the default product behaviour.
TEST_F(NexusBackgroundPanelHostTest, EnabledFollowsBackgroundModePref) {
  local_state()->SetBoolean(prefs::kBackgroundModeEnabled, true);
  EXPECT_TRUE(NexusBackgroundPanelHost::IsBackgroundPanelEnabled());
  local_state()->SetBoolean(prefs::kBackgroundModeEnabled, false);
  EXPECT_FALSE(NexusBackgroundPanelHost::IsBackgroundPanelEnabled());
}

TEST_F(NexusBackgroundPanelHostTest, ParkThenTakeRoundTripsTheSameContents) {
  ASSERT_TRUE(host());
  EXPECT_FALSE(host()->has_parked());
  EXPECT_EQ(nullptr, host()->Take());

  std::unique_ptr<content::WebContents> contents = MakeContents();
  content::WebContents* raw = contents.get();
  host()->Park(std::move(contents));
  EXPECT_TRUE(host()->has_parked());
  EXPECT_EQ(raw, host()->parked_for_testing());

  std::unique_ptr<content::WebContents> taken = host()->Take();
  EXPECT_EQ(raw, taken.get());
  EXPECT_FALSE(host()->has_parked());
  EXPECT_EQ(nullptr, host()->Take());
}

TEST_F(NexusBackgroundPanelHostTest, ParkReplacesAnEarlierParkedContents) {
  ASSERT_TRUE(host());
  host()->Park(MakeContents());
  std::unique_ptr<content::WebContents> second = MakeContents();
  content::WebContents* raw_second = second.get();
  host()->Park(std::move(second));
  EXPECT_EQ(raw_second, host()->parked_for_testing());
  // The harness checks for leaked RenderWidgetHosts before the profile (and
  // so the service) is torn down: release the parked page here.
  host()->Shutdown();
}

TEST_F(NexusBackgroundPanelHostTest, ShutdownDropsTheParkedContents) {
  ASSERT_TRUE(host());
  host()->Park(MakeContents());
  host()->Shutdown();
  EXPECT_FALSE(host()->has_parked());
}

// A parked page whose renderer died is dropped, so the next window loads a
// fresh page instead of adopting a dead one.
TEST_F(NexusBackgroundPanelHostTest, RendererCrashDropsTheParkedContents) {
  ASSERT_TRUE(host());
  host()->Park(MakeContents());
  ASSERT_TRUE(host()->has_parked());
  static_cast<content::MockRenderProcessHost*>(
      host()->parked_for_testing()->GetPrimaryMainFrame()->GetProcess())
      ->SimulateCrash();
  EXPECT_FALSE(host()->has_parked());
}

TEST_F(NexusBackgroundPanelHostTest, NoServiceForOffTheRecordProfile) {
  Profile* otr = profile()->GetPrimaryOTRProfile(/*create_if_needed=*/true);
  EXPECT_EQ(nullptr, NexusBackgroundPanelHostFactory::GetForProfile(otr));
  EXPECT_EQ(nullptr, NexusBackgroundPanelHostFactory::GetForProfile(nullptr));
}

}  // namespace
}  // namespace nexus
