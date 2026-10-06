// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/alarms/nexus_alarm_service.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "base/time/clock.h"
#include "base/values.h"
#include "chrome/browser/prefs/browser_prefs.h"
#include "chrome/test/base/testing_profile.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/testing_pref_service.h"
#include "components/sync_preferences/testing_pref_service_syncable.h"
#include "content/public/test/browser_task_environment.h"
#include "custom_browser/browser/nexus/api/alarms/nexus_alarms_bridge_adapter.h"
#include "custom_browser/browser/nexus/api/llm/llm_pref_names.h"
#include "custom_browser/browser/nexus/api/llm/llm_pref_registration.h"
#include "custom_browser/browser/nexus/launcher/nexus_launcher_delegate.h"
#include "custom_browser/browser/nexus/nexus_bridge_response_delegate.h"
#include "custom_browser/browser/nexus/nexus_test_support.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

class NexusAlarmServiceTest : public testing::Test {
 protected:
  NexusAlarmServiceTest() {
    prefs_.registry()->RegisterDictionaryPref(prefs::kNexusAlarms);
  }

  std::unique_ptr<NexusAlarmService> MakeService() {
    return std::make_unique<NexusAlarmService>(
        &prefs_, task_environment_.GetMockClock(),
        base::BindRepeating([](int* wakes) { ++*wakes; }, &wakes_));
  }

  double NowMs() {
    return task_environment_.GetMockClock()
        ->Now()
        .InMillisecondsFSinceUnixEpoch();
  }

  content::BrowserTaskEnvironment task_environment_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  TestingPrefServiceSimple prefs_;
  int wakes_ = 0;
};

TEST_F(NexusAlarmServiceTest, AFireGoesToConnectedSinks) {
  auto service = MakeService();
  std::vector<AlarmFire> seen;
  service->AddSink(base::BindRepeating(
      [](std::vector<AlarmFire>* out, const AlarmFire& fire) {
        out->push_back(fire);
      },
      &seen));
  const double due = NowMs() + 5000;
  ASSERT_FALSE(service->Create("tick", due, std::nullopt));

  task_environment_.FastForwardBy(base::Seconds(4));
  EXPECT_TRUE(seen.empty());
  task_environment_.FastForwardBy(base::Seconds(2));
  ASSERT_EQ(seen.size(), 1u);
  EXPECT_EQ(seen[0].name, "tick");
  EXPECT_EQ(seen[0].scheduled_ms, due);
  EXPECT_EQ(wakes_, 0);
  EXPECT_TRUE(service->alarms().empty());  // one-shot
}

TEST_F(NexusAlarmServiceTest, WithNoRuntimeAFireIsQueuedAndWakesOne) {
  auto service = MakeService();
  ASSERT_FALSE(service->Create("job", std::nullopt, 60'000.0));
  task_environment_.FastForwardBy(base::Minutes(1));
  EXPECT_EQ(wakes_, 1);
  std::vector<AlarmFire> pending = service->TakePending();
  ASSERT_EQ(pending.size(), 1u);
  EXPECT_EQ(pending[0].name, "job");
  EXPECT_TRUE(service->TakePending().empty());  // drained
  // Periodic: still scheduled, a minute on.
  ASSERT_TRUE(service->alarms().contains("job"));
}

// Persisted across a "restart": a new service over the same prefs sees the
// alarm, and one that came due while it was down fires once at startup.
TEST_F(NexusAlarmServiceTest, AlarmsPersistAndMissedOnesFireOnceAtStartup) {
  {
    auto service = MakeService();
    ASSERT_FALSE(service->Create("hourly", NowMs() + 1000, 3'600'000.0));
  }
  ASSERT_TRUE(prefs_.GetDict(prefs::kNexusAlarms).FindDict("hourly"));

  // The browser is off for five hours: no service, no timer.
  task_environment_.AdvanceClock(base::Hours(5));

  auto restarted = MakeService();
  task_environment_.RunUntilIdle();
  std::vector<AlarmFire> pending = restarted->TakePending();
  ASSERT_EQ(pending.size(), 1u);  // once, not five times
  EXPECT_EQ(pending[0].name, "hourly");
  EXPECT_EQ(wakes_, 1);
  EXPECT_GT(restarted->alarms().at("hourly").when_ms, NowMs());
}

TEST_F(NexusAlarmServiceTest, ClearRemovesTheAlarmAndItsQueuedFires) {
  auto service = MakeService();
  ASSERT_FALSE(service->Create("a", NowMs(), 60'000.0));
  task_environment_.RunUntilIdle();
  EXPECT_TRUE(service->Clear("a"));
  EXPECT_FALSE(service->Clear("a"));
  EXPECT_TRUE(service->TakePending().empty());
  EXPECT_TRUE(prefs_.GetDict(prefs::kNexusAlarms).empty());
}

// ---- The adapter -----------------------------------------------------------

class RecordingDelegate : public NexusBridgeResponseDelegate {
 public:
  base::Value last_success;
  std::string last_error_code;
  std::vector<std::pair<std::string, base::Value>> events;

  void SendSuccessResponse(const std::string& /*id*/,
                           const base::DictValue& /*route*/,
                           base::Value result) override {
    last_success = std::move(result);
  }
  void SendBinaryResponse(const std::string& /*id*/,
                          const base::DictValue& /*route*/,
                          base::Value /*result*/,
                          mojo_base::BigBuffer /*data*/,
                          std::string_view /*field*/) override {}
  void SendErrorResponse(const std::string& /*id*/,
                         const base::DictValue& /*route*/,
                         const std::string& code,
                         const std::string& /*message*/) override {
    last_error_code = code;
  }
  void SendEvent(const base::DictValue& route, base::Value payload) override {
    const std::string* action = route.FindString("action");
    events.emplace_back(action ? *action : std::string(), std::move(payload));
  }
};

TEST_F(NexusAlarmServiceTest, TheAdapterCreatesListsAndPushesOnAlarm) {
  auto service = MakeService();
  RecordingDelegate delegate;
  {
    // A fire before any adapter existed is queued for the first list.
    ASSERT_FALSE(service->Create("early", NowMs(), std::nullopt));
    task_environment_.RunUntilIdle();
  }
  NexusAlarmsBridgeAdapter adapter(&delegate, service.get());
  adapter.OnBridgeConnectionOpened();  // NexusBridgePortService::CreatePortPair
  base::DictValue route;
  route.Set("namespace", "nexus");
  route.Set("capability", "alarms");

  base::DictValue create;
  create.Set("name", "poll");
  create.Set("periodMs", 30'000.0);
  EXPECT_TRUE(adapter.Dispatch("create", "req", route, create, {}));
  task_environment_.RunUntilIdle();

  EXPECT_TRUE(adapter.Dispatch("list", "req", route, base::DictValue(), {}));
  task_environment_.RunUntilIdle();
  const base::DictValue& listed = delegate.last_success.GetDict();
  ASSERT_EQ(listed.FindList("alarms")->size(), 1u);
  ASSERT_EQ(listed.FindList("fired")->size(), 1u);
  EXPECT_EQ(*(*listed.FindList("fired"))[0].GetDict().FindString("name"),
            "early");

  // With the adapter connected, a fire is an event — no wake.
  const int wakes_before = wakes_;
  task_environment_.FastForwardBy(base::Seconds(30));
  ASSERT_EQ(delegate.events.size(), 1u);
  EXPECT_EQ(delegate.events[0].first, "onAlarm");
  EXPECT_EQ(*delegate.events[0].second.GetDict().FindString("name"), "poll");
  EXPECT_EQ(wakes_, wakes_before);

  base::DictValue bad;
  bad.Set("name", "x");
  bad.Set("periodMs", 10.0);
  EXPECT_TRUE(adapter.Dispatch("create", "req", route, bad, {}));
  task_environment_.RunUntilIdle();
  EXPECT_EQ(delegate.last_error_code, "INVALID_ARGS");
}

// The guest's port died (a crashed or reloading webview): the adapter lives on
// with its port service, but an event sent now is dropped. So the fire must
// queue and wake a runtime, not vanish into the dead port; the next
// connection makes the adapter a sink again.
TEST_F(NexusAlarmServiceTest, AFireWhileTheConnectionIsDownIsQueued) {
  auto service = MakeService();
  RecordingDelegate delegate;
  NexusAlarmsBridgeAdapter adapter(&delegate, service.get());
  adapter.OnBridgeConnectionOpened();
  adapter.OnBridgeConnectionClosed();

  ASSERT_FALSE(service->Create("once", NowMs() + 1000, std::nullopt));
  task_environment_.FastForwardBy(base::Seconds(2));
  EXPECT_TRUE(delegate.events.empty());
  EXPECT_EQ(wakes_, 1);
  std::vector<AlarmFire> pending = service->TakePending();
  ASSERT_EQ(pending.size(), 1u);
  EXPECT_EQ(pending[0].name, "once");

  // Reconnected: fires are events again, no wake. Opening twice adds one sink.
  adapter.OnBridgeConnectionOpened();
  adapter.OnBridgeConnectionOpened();
  ASSERT_FALSE(service->Create("again", NowMs() + 1000, std::nullopt));
  task_environment_.FastForwardBy(base::Seconds(2));
  ASSERT_EQ(delegate.events.size(), 1u);
  EXPECT_EQ(*delegate.events[0].second.GetDict().FindString("name"), "again");
  EXPECT_EQ(wakes_, 1);
}

// A fire that finds no runtime boots the runtime of the profile it fired in,
// not the last-used profile's.
class RecordingLauncherDelegate : public NexusLauncherDelegate {
 public:
  void OpenOrFocusNexusWindow() override { ++unscoped_opens; }
  void OpenOrFocusNexusWindowForProfile(Profile* profile) override {
    opened_for.push_back(profile);
  }
  void OpenOrFocusNormalWindow() override {}
  void ConfirmQuit(base::OnceClosure proceed) override {}
  void Quit() override {}

  int unscoped_opens = 0;
  std::vector<Profile*> opened_for;
};

TEST_F(NexusAlarmServiceTest, AWakeOpensTheFiringProfilesWindow) {
  auto prefs = std::make_unique<sync_preferences::TestingPrefServiceSyncable>();
  RegisterUserProfilePrefs(prefs->registry());
  RegisterNexusProfilePrefs(prefs->registry());
  TestingProfile::Builder builder;
  builder.SetPrefService(std::move(prefs));
  test::AddGlicDisablingFactory(builder);
  std::unique_ptr<TestingProfile> profile = builder.Build();

  RecordingLauncherDelegate launcher;
  NexusLauncherDelegate* previous = GetNexusLauncherDelegate();
  SetNexusLauncherDelegate(&launcher);
  NexusAlarmServiceFactory::WakeRuntimeForProfile(profile.get());
  SetNexusLauncherDelegate(previous);
  ASSERT_EQ(launcher.opened_for.size(), 1u);
  EXPECT_EQ(launcher.opened_for[0], profile.get());
  EXPECT_EQ(launcher.unscoped_opens, 0);
}

}  // namespace
}  // namespace nexus
