// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/nexus_bridge_bootstrap_host.h"

#include <memory>

#include "base/test/task_environment.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "chrome/test/base/testing_profile.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "custom_browser/browser/nexus/api/llm/llm_broker_factory.h"
#include "custom_browser/browser/nexus/nexus_test_support.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace nexus {
namespace {

// GuestReady probes the console host on the thread pool and posts the
// bootstrap on the reply; the reply must find the same document still there.
// Thread-pool tasks are queued, so the probe runs only at RunUntilIdle() —
// never inside a navigation's run loop.
class NexusBridgeBootstrapHostTest : public ChromeRenderViewHostTestHarness {
 protected:
  NexusBridgeBootstrapHostTest()
      : ChromeRenderViewHostTestHarness(
            base::test::TaskEnvironment::ThreadPoolExecutionMode::QUEUED) {}

  TestingProfile::TestingFactories GetTestingFactories() const override {
    return test::GlicDisablingTestingFactories();
  }

  // The keyed-service factory registers the Nexus profile prefs the
  // bootstrap message reads, but only on profiles built after it exists.
  std::unique_ptr<TestingProfile> CreateTestingProfile() override {
    LlmBrokerFactory::GetInstance();
    return ChromeRenderViewHostTestHarness::CreateTestingProfile();
  }

  void SetUp() override {
    ChromeRenderViewHostTestHarness::SetUp();
    NavigateAndCommit(GURL("https://guest.test/"));
  }

  std::unique_ptr<NexusBridgeBootstrapHost> MakeHost() {
    return std::make_unique<NexusBridgeBootstrapHost>(main_rfh(), profile());
  }
};

TEST_F(NexusBridgeBootstrapHostTest, PostsTheBootstrapToALiveDocument) {
  std::unique_ptr<NexusBridgeBootstrapHost> host = MakeHost();
  host->GuestReady();
  task_environment()->RunUntilIdle();
  EXPECT_TRUE(host->bootstrap_posted_for_testing());
}

// A same-site navigation: the next document may reuse the RenderFrameHost.
TEST_F(NexusBridgeBootstrapHostTest, SkipsTheBootstrapAfterANavigation) {
  std::unique_ptr<NexusBridgeBootstrapHost> host = MakeHost();
  host->GuestReady();
  NavigateAndCommit(GURL("https://guest.test/next"));
  task_environment()->RunUntilIdle();
  EXPECT_FALSE(host->bootstrap_posted_for_testing());
}

TEST_F(NexusBridgeBootstrapHostTest, SkipsTheBootstrapAfterTheFrameIsGone) {
  std::unique_ptr<NexusBridgeBootstrapHost> host = MakeHost();
  host->GuestReady();
  DeleteContents();
  task_environment()->RunUntilIdle();
  EXPECT_FALSE(host->bootstrap_posted_for_testing());
}

}  // namespace
}  // namespace nexus
