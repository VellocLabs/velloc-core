// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/app_info/nexus_render_cache_purger.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/values.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/navigation_simulator.h"
#include "content/public/test/web_contents_tester.h"
#include "custom_browser/browser/nexus/api/app_info/nexus_app_info_bridge_adapter.h"
#include "custom_browser/browser/nexus/nexus_bridge_response_delegate.h"
#include "custom_browser/browser/nexus/nexus_test_support.h"
#include "custom_browser/browser/nexus/session/nexus_session_tab_tag.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace nexus {
namespace {

using Purger = NexusRenderCachePurger;

class RecordingDelegate : public NexusBridgeResponseDelegate {
 public:
  int success_count = 0;
  int error_count = 0;
  base::Value last_success;
  std::string last_error_code;

  void SendSuccessResponse(const std::string& /*id*/,
                           const base::DictValue& /*route*/,
                           base::Value result) override {
    ++success_count;
    last_success = std::move(result);
  }
  void SendBinaryResponse(const std::string& /*id*/,
                          const base::DictValue& /*route*/,
                          base::Value /*result*/,
                          mojo_base::BigBuffer /*data*/,
                          std::string_view /*result_binary_field*/) override {}
  void SendErrorResponse(const std::string& /*id*/,
                         const base::DictValue& /*route*/,
                         const std::string& code,
                         const std::string& /*message*/) override {
    ++error_count;
    last_error_code = code;
  }
  void SendEvent(const base::DictValue& /*route*/,
                 base::Value /*payload*/) override {}
};

struct RendererSend {
  raw_ptr<content::RenderProcessHost> host;
  base::MemoryPressureLevel level;
};

class NexusRenderCachePurgerTest : public ChromeRenderViewHostTestHarness {
 protected:
  NexusRenderCachePurgerTest()
      : ChromeRenderViewHostTestHarness(
            base::test::TaskEnvironment::TimeSource::MOCK_TIME) {}

  TestingProfile::TestingFactories GetTestingFactories() const override {
    return test::GlicDisablingTestingFactories();
  }

  void SetUp() override {
    ChromeRenderViewHostTestHarness::SetUp();
    Purger::SetSendersForTesting(
        base::BindRepeating(
            [](std::vector<RendererSend>* sends,
               content::RenderProcessHost* host,
               base::MemoryPressureLevel level) {
              sends->push_back({host, level});
            },
            &renderer_sends_),
        base::BindRepeating(
            [](std::vector<base::MemoryPressureLevel>* sends,
               base::MemoryPressureLevel level) {
              sends->push_back(level);
              return 1;
            },
            &gpu_sends_));
  }

  void TearDown() override {
    renderer_sends_.clear();
    Purger::SetSendersForTesting({}, {});
    ChromeRenderViewHostTestHarness::TearDown();
  }

  std::unique_ptr<content::WebContents> MakeTab(bool tagged) {
    std::unique_ptr<content::WebContents> contents =
        content::WebContentsTester::CreateTestWebContents(profile(), nullptr);
    content::NavigationSimulator::NavigateAndCommitFromBrowser(
        contents.get(), GURL("https://example.com/"));
    if (tagged) {
      NexusSessionTabTag::AttachTo(contents.get(), "session-a");
    }
    return contents;
  }

  // Dispatches appInfo.purgeRenderCaches and lets the reply land.
  void DispatchPurge(base::DictValue payload) {
    NexusAppInfoBridgeAdapter adapter(&delegate_, profile());
    base::DictValue route;
    route.Set("namespace", "nexus");
    route.Set("capability", "appInfo");
    route.Set("action", "purgeRenderCaches");
    ASSERT_TRUE(adapter.Dispatch("purgeRenderCaches", "1", route, payload,
                                 /*ports=*/{}));
    task_environment()->RunUntilIdle();
  }

  std::vector<RendererSend> renderer_sends_;
  std::vector<base::MemoryPressureLevel> gpu_sends_;
  RecordingDelegate delegate_;
};

TEST_F(NexusRenderCachePurgerTest, SendsToEachSessionTabProcessOnce) {
  std::unique_ptr<content::WebContents> tab = MakeTab(/*tagged=*/true);

  const Purger::Result result = Purger::Purge(
      profile(), Purger::kAgentTabs, base::MEMORY_PRESSURE_LEVEL_MODERATE);

  EXPECT_EQ(result.purged, static_cast<uint32_t>(Purger::kAgentTabs));
  EXPECT_EQ(result.throttled, 0u);
  EXPECT_EQ(result.agent_tab_processes, 1);
  EXPECT_EQ(result.guest_processes, 0);
  ASSERT_EQ(renderer_sends_.size(), 1u);
  EXPECT_EQ(renderer_sends_[0].host.get(),
            tab->GetPrimaryMainFrame()->GetProcess());
  EXPECT_EQ(renderer_sends_[0].level, base::MEMORY_PRESSURE_LEVEL_MODERATE);
  EXPECT_TRUE(gpu_sends_.empty());
}

TEST_F(NexusRenderCachePurgerTest, UntaggedTabsAreLeftAlone) {
  std::unique_ptr<content::WebContents> tab = MakeTab(/*tagged=*/false);

  const Purger::Result result =
      Purger::Purge(profile(), Purger::kGuest | Purger::kAgentTabs,
                    base::MEMORY_PRESSURE_LEVEL_CRITICAL);

  EXPECT_EQ(result.purged,
            static_cast<uint32_t>(Purger::kGuest | Purger::kAgentTabs));
  EXPECT_EQ(result.agent_tab_processes, 0);
  EXPECT_EQ(result.guest_processes, 0);
  EXPECT_TRUE(renderer_sends_.empty());
}

TEST_F(NexusRenderCachePurgerTest, GpuTargetSendsOnlyToGpu) {
  std::unique_ptr<content::WebContents> tab = MakeTab(/*tagged=*/true);

  const Purger::Result result = Purger::Purge(
      profile(), Purger::kGpu, base::MEMORY_PRESSURE_LEVEL_CRITICAL);

  EXPECT_EQ(result.purged, static_cast<uint32_t>(Purger::kGpu));
  EXPECT_EQ(result.gpu_processes, 1);
  EXPECT_TRUE(renderer_sends_.empty());
  ASSERT_EQ(gpu_sends_.size(), 1u);
  EXPECT_EQ(gpu_sends_[0], base::MEMORY_PRESSURE_LEVEL_CRITICAL);
}

TEST_F(NexusRenderCachePurgerTest, RateLimitIsPerTarget) {
  Purger::Purge(profile(), Purger::kGpu, base::MEMORY_PRESSURE_LEVEL_CRITICAL);

  task_environment()->FastForwardBy(Purger::kMinInterval / 2);
  Purger::Result result =
      Purger::Purge(profile(), Purger::kGpu | Purger::kAgentTabs,
                    base::MEMORY_PRESSURE_LEVEL_CRITICAL);
  EXPECT_EQ(result.throttled, static_cast<uint32_t>(Purger::kGpu));
  EXPECT_EQ(result.purged, static_cast<uint32_t>(Purger::kAgentTabs));
  EXPECT_EQ(gpu_sends_.size(), 1u);

  task_environment()->FastForwardBy(Purger::kMinInterval / 2);
  result = Purger::Purge(profile(), Purger::kGpu,
                         base::MEMORY_PRESSURE_LEVEL_CRITICAL);
  EXPECT_EQ(result.purged, static_cast<uint32_t>(Purger::kGpu));
  EXPECT_EQ(result.throttled, 0u);
  EXPECT_EQ(gpu_sends_.size(), 2u);
}

TEST_F(NexusRenderCachePurgerTest, ActionDefaultsToGuestAndGpuCritical) {
  DispatchPurge(base::DictValue());

  ASSERT_EQ(delegate_.error_count, 0);
  ASSERT_EQ(delegate_.success_count, 1);
  const base::DictValue& reply = delegate_.last_success.GetDict();
  base::ListValue expected;
  expected.Append("guest");
  expected.Append("gpu");
  EXPECT_EQ(*reply.FindList("purged"), expected);
  EXPECT_TRUE(reply.FindList("throttled")->empty());
  EXPECT_EQ(reply.FindInt("guestProcesses"), 0);
  EXPECT_EQ(reply.FindInt("agentTabProcesses"), 0);
  EXPECT_EQ(reply.FindInt("gpuProcesses"), 1);
  ASSERT_EQ(gpu_sends_.size(), 1u);
  EXPECT_EQ(gpu_sends_[0], base::MEMORY_PRESSURE_LEVEL_CRITICAL);
}

TEST_F(NexusRenderCachePurgerTest, ActionHonoursTargetsAndLevel) {
  std::unique_ptr<content::WebContents> tab = MakeTab(/*tagged=*/true);
  base::DictValue payload;
  base::ListValue targets;
  targets.Append("agentTabs");
  payload.Set("targets", std::move(targets));
  payload.Set("level", "moderate");
  DispatchPurge(std::move(payload));

  ASSERT_EQ(delegate_.success_count, 1);
  EXPECT_EQ(delegate_.last_success.GetDict().FindInt("agentTabProcesses"), 1);
  ASSERT_EQ(renderer_sends_.size(), 1u);
  EXPECT_EQ(renderer_sends_[0].level, base::MEMORY_PRESSURE_LEVEL_MODERATE);
  EXPECT_TRUE(gpu_sends_.empty());
}

TEST_F(NexusRenderCachePurgerTest, ActionRefusesUnknownTargetOrLevel) {
  base::DictValue bad_target;
  base::ListValue targets;
  targets.Append("gpu");
  targets.Append("everything");
  bad_target.Set("targets", std::move(targets));
  DispatchPurge(std::move(bad_target));
  EXPECT_EQ(delegate_.error_count, 1);
  EXPECT_EQ(delegate_.last_error_code, "INVALID_ARGS");

  base::DictValue bad_level;
  bad_level.Set("level", "low");
  DispatchPurge(std::move(bad_level));
  EXPECT_EQ(delegate_.error_count, 2);
  EXPECT_EQ(delegate_.last_error_code, "INVALID_ARGS");

  EXPECT_EQ(delegate_.success_count, 0);
  EXPECT_TRUE(gpu_sends_.empty());
  EXPECT_TRUE(renderer_sends_.empty());
}

}  // namespace
}  // namespace nexus
