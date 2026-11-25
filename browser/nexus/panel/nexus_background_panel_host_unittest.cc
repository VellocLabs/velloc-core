// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

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
#include "content/public/browser/site_instance.h"
#include "content/public/browser/storage_partition_config.h"
#include "content/public/browser/visibility.h"
#include "content/public/test/mock_render_process_host.h"
#include "content/public/test/navigation_simulator.h"
#include "content/public/test/test_renderer_host.h"
#include "content/public/test/web_contents_tester.h"
#include "custom_browser/browser/nexus/nexus_test_support.h"
#include "custom_browser/browser/nexus/panel/nexus_parked_page_workers.h"
#include "custom_browser/chromium_hooks/nexus_guest_groups.h"
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

  // Attaches a stand-in guest to `outer` under a new child frame.
  void AttachGuest(content::WebContents* outer) {
    content::NavigationSimulator::NavigateAndCommitFromBrowser(
        outer, GURL("about:blank"));
    content::RenderFrameHost* child =
        content::RenderFrameHostTester::For(outer->GetPrimaryMainFrame())
            ->AppendChild("guest");
    content::RenderFrameHostTester::For(child)->InitializeRenderFrameIfNeeded();
    outer->AttachInnerWebContents(MakeContents(), child,
                                  /*is_full_page=*/false);
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

// The runtime host page is one never-shown page per profile, started once
// and gone when stopped.
TEST_F(NexusBackgroundPanelHostTest, RuntimeHostStartsOnceAndStops) {
  ASSERT_TRUE(host());
  EXPECT_FALSE(host()->has_runtime_host());
  const GURL url("about:blank");
  host()->set_runtime_host_url_for_testing(url);
  host()->StartRuntimeHost();
  content::WebContents* page = host()->runtime_host_for_testing();
  ASSERT_TRUE(page);
  EXPECT_EQ(url, page->GetVisibleURL());
  EXPECT_EQ("nexusRuntimeHost=1",
            NexusBackgroundPanelHost::RuntimeHostUrl().query());
  EXPECT_EQ(content::Visibility::HIDDEN, page->GetVisibility());
  host()->StartRuntimeHost();
  EXPECT_EQ(page, host()->runtime_host_for_testing());
  host()->StopRuntimeHost();
  EXPECT_FALSE(host()->has_runtime_host());
}

// Parking and taking a window's page leaves the runtime host page alone.
TEST_F(NexusBackgroundPanelHostTest, RuntimeHostOutlivesTheParkedPage) {
  ASSERT_TRUE(host());
  host()->set_runtime_host_url_for_testing(GURL("about:blank"));
  host()->StartRuntimeHost();
  content::WebContents* page = host()->runtime_host_for_testing();
  host()->Park(MakeContents());
  static_cast<content::MockRenderProcessHost*>(
      host()->parked_for_testing()->GetPrimaryMainFrame()->GetProcess())
      ->SimulateCrash();
  EXPECT_FALSE(host()->has_parked());
  EXPECT_EQ(page, host()->runtime_host_for_testing());
  host()->Shutdown();
  EXPECT_FALSE(host()->has_runtime_host());
}

// A runtime host page whose renderer is gone does not count as running, and
// the next start replaces it.
TEST_F(NexusBackgroundPanelHostTest, CrashedRuntimeHostIsReplacedOnStart) {
  ASSERT_TRUE(host());
  host()->set_runtime_host_url_for_testing(GURL("about:blank"));
  host()->set_runtime_host_reload_enabled_for_testing(false);
  host()->StartRuntimeHost();
  content::WebContents* page = host()->runtime_host_for_testing();
  ASSERT_TRUE(page);
  static_cast<content::MockRenderProcessHost*>(
      page->GetPrimaryMainFrame()->GetProcess())
      ->SimulateCrash();
  EXPECT_FALSE(host()->has_runtime_host());
  EXPECT_FALSE(host()->runs_without_window());
  host()->StartRuntimeHost();
  ASSERT_TRUE(host()->has_runtime_host());
  EXPECT_FALSE(host()->runtime_host_for_testing()->IsCrashed());
  host()->StopRuntimeHost();
  EXPECT_FALSE(host()->has_runtime_host());
}

// A parked page counts as idle only while the runtime host page and its guest
// are live, and the state-change callbacks fire on every step the voter must
// re-check. (P-2)
TEST_F(NexusBackgroundPanelHostTest, ParkedPageIsIdleOnlyBesideALiveRuntime) {
  ASSERT_TRUE(host());
  int changes = 0;
  base::CallbackListSubscription subscription =
      NexusBackgroundPanelHost::AddStateChangedCallback(
          base::BindRepeating([](int* count) { ++*count; }, &changes));
  EXPECT_FALSE(NexusBackgroundPanelHost::IsParkedAndIdle(nullptr));

  std::unique_ptr<content::WebContents> contents = MakeContents();
  content::WebContents* raw = contents.get();
  EXPECT_FALSE(NexusBackgroundPanelHost::IsParkedAndIdle(raw));

  host()->Park(std::move(contents));
  EXPECT_EQ(1, changes);
  EXPECT_FALSE(NexusBackgroundPanelHost::IsParkedAndIdle(raw))
      << "no runtime host page: the parked page still hosts the runtime";

  host()->set_runtime_host_url_for_testing(GURL("about:blank"));
  host()->set_runtime_host_reload_enabled_for_testing(false);
  host()->StartRuntimeHost();
  EXPECT_GE(changes, 2);
  EXPECT_FALSE(NexusBackgroundPanelHost::IsParkedAndIdle(raw))
      << "the host page's guest, which holds the runtime, is not attached";

  const int before_crash = changes;
  static_cast<content::MockRenderProcessHost*>(
      host()->runtime_host_for_testing()->GetPrimaryMainFrame()->GetProcess())
      ->SimulateCrash();
  EXPECT_GT(changes, before_crash);
  EXPECT_FALSE(NexusBackgroundPanelHost::IsParkedAndIdle(raw))
      << "a runtime host page whose renderer is gone runs nothing";

  host()->StopRuntimeHost();
  host()->StartRuntimeHost();
  EXPECT_FALSE(NexusBackgroundPanelHost::IsParkedAndIdle(raw));

  const int before_attach = changes;
  AttachGuest(host()->runtime_host_for_testing());
  EXPECT_GT(changes, before_attach);
  EXPECT_FALSE(NexusBackgroundPanelHost::IsParkedAndIdle(raw))
      << "the runtime host guest has not loaded anything yet";
  content::WebContents* host_guest =
      host()->runtime_host_for_testing()->GetInnerWebContents().front();
  const int before_load = changes;
  content::NavigationSimulator::NavigateAndCommitFromBrowser(
      host_guest, GURL("https://runtime.test/"));
  EXPECT_GT(changes, before_load);
  EXPECT_TRUE(NexusBackgroundPanelHost::IsParkedAndIdle(raw))
      << "the runtime host guest is live and no worker runs in the page";

  const int before_guest_crash = changes;
  static_cast<content::MockRenderProcessHost*>(
      host_guest->GetPrimaryMainFrame()->GetProcess())
      ->SimulateCrash();
  EXPECT_GT(changes, before_guest_crash)
      << "the runtime's own renderer going away is reported";
  EXPECT_FALSE(NexusBackgroundPanelHost::IsParkedAndIdle(raw));

  const int before_take = changes;
  std::unique_ptr<content::WebContents> taken = host()->Take();
  EXPECT_GT(changes, before_take);
  EXPECT_FALSE(NexusBackgroundPanelHost::IsParkedAndIdle(raw))
      << "a page back in a window is not parked";
  host()->StopRuntimeHost();
}

// The worker tracker counts workers of frames in the parked page's guests,
// and their nested workers, and nothing the page itself or another page runs.
TEST_F(NexusBackgroundPanelHostTest, ParkedPageWorkersCountOnlyTheGuests) {
  ASSERT_TRUE(host());
  std::unique_ptr<content::WebContents> contents = MakeContents();
  content::WebContents* raw = contents.get();
  AttachGuest(raw);
  host()->Park(std::move(contents));
  NexusParkedPageWorkers* workers = host()->parked_workers_for_testing();
  ASSERT_TRUE(workers);
  int changes = 0;
  base::CallbackListSubscription subscription =
      NexusBackgroundPanelHost::AddStateChangedCallback(
          base::BindRepeating([](int* count) { ++*count; }, &changes));

  content::WebContents* guest = raw->GetInnerWebContents().front();
  const blink::DedicatedWorkerToken from_page, from_guest, nested;
  const url::Origin origin;
  const content::ChildProcessId process;
  workers->OnWorkerCreated(from_page, process, origin,
                           raw->GetPrimaryMainFrame()->GetGlobalId());
  EXPECT_FALSE(workers->has_workers()) << "the page's own frame is no guest";
  EXPECT_EQ(0, changes);

  workers->OnWorkerCreated(from_guest, process, origin,
                           guest->GetPrimaryMainFrame()->GetGlobalId());
  EXPECT_TRUE(workers->has_workers());
  EXPECT_EQ(1, changes);
  workers->OnWorkerCreated(nested, process, origin, from_guest);
  workers->OnBeforeWorkerDestroyed(from_guest,
                                   guest->GetPrimaryMainFrame()->GetGlobalId());
  EXPECT_TRUE(workers->has_workers()) << "the nested worker still runs";
  EXPECT_EQ(1, changes);
  EXPECT_FALSE(NexusBackgroundPanelHost::IsParkedAndIdle(raw));
  workers->OnBeforeWorkerDestroyed(nested, from_guest);
  EXPECT_FALSE(workers->has_workers());
  EXPECT_EQ(2, changes);
  host()->Shutdown();
}

// The group keys are three distinct addresses.
TEST_F(NexusBackgroundPanelHostTest, GuestGroupKeysAreDistinct) {
  using custom_browser::NexusGuestGroupsSlot;
  using custom_browser::NexusGuestGroupsUserDataKey;
  const void* enabled =
      NexusGuestGroupsUserDataKey(NexusGuestGroupsSlot::kEnabled);
  const void* runtime =
      NexusGuestGroupsUserDataKey(NexusGuestGroupsSlot::kRuntime);
  const void* ui = NexusGuestGroupsUserDataKey(NexusGuestGroupsSlot::kUi);
  EXPECT_NE(enabled, runtime);
  EXPECT_NE(enabled, ui);
  EXPECT_NE(runtime, ui);
}

// The "nexus" partition has two process groups while the service lives: one
// SiteInstance each, unrelated to one another, and none for other partitions.
TEST_F(NexusBackgroundPanelHostTest, GuestGroupsLiveWithTheService) {
  const auto nexus_config = content::StoragePartitionConfig::Create(
      profile(), "owner", "nexus", /*in_memory=*/false);
  const auto other_config = content::StoragePartitionConfig::Create(
      profile(), "owner", "other", /*in_memory=*/false);
  using custom_browser::GetNexusGuestGroupSiteInstance;
  using custom_browser::NexusGuestGroup;
  EXPECT_FALSE(GetNexusGuestGroupSiteInstance(profile(), nexus_config,
                                              NexusGuestGroup::kRuntime));

  ASSERT_TRUE(host());
  scoped_refptr<content::SiteInstance> runtime = GetNexusGuestGroupSiteInstance(
      profile(), nexus_config, NexusGuestGroup::kRuntime);
  scoped_refptr<content::SiteInstance> ui = GetNexusGuestGroupSiteInstance(
      profile(), nexus_config, NexusGuestGroup::kUi);
  ASSERT_TRUE(runtime);
  ASSERT_TRUE(ui);
  EXPECT_EQ(runtime, GetNexusGuestGroupSiteInstance(profile(), nexus_config,
                                                    NexusGuestGroup::kRuntime));
  EXPECT_EQ(ui, GetNexusGuestGroupSiteInstance(profile(), nexus_config,
                                               NexusGuestGroup::kUi));
  EXPECT_NE(runtime, ui);
  EXPECT_FALSE(runtime->IsRelatedSiteInstance(ui.get()));
  EXPECT_FALSE(GetNexusGuestGroupSiteInstance(profile(), other_config,
                                              NexusGuestGroup::kRuntime));

  runtime.reset();
  ui.reset();
  host()->Shutdown();
  EXPECT_FALSE(GetNexusGuestGroupSiteInstance(profile(), nexus_config,
                                              NexusGuestGroup::kRuntime));
}

}  // namespace
}  // namespace nexus
