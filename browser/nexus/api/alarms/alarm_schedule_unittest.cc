// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/alarms/alarm_schedule.h"

#include <optional>
#include <string>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

constexpr double kMinute = 60'000;

TEST(AlarmScheduleTest, MakeAlarmSpecNormalizesAndValidates) {
  AlarmSpec spec;
  EXPECT_FALSE(MakeAlarmSpec("daily", std::nullopt, kMinute, 1000, &spec));
  EXPECT_EQ(spec.when_ms, 1000 + kMinute);  // one period from now
  EXPECT_EQ(spec.period_ms, kMinute);

  EXPECT_FALSE(MakeAlarmSpec("once", 5000.0, std::nullopt, 1000, &spec));
  EXPECT_EQ(spec.when_ms, 5000);
  EXPECT_FALSE(spec.period_ms.has_value());

  EXPECT_TRUE(MakeAlarmSpec("", 5000.0, std::nullopt, 0, &spec).has_value());
  EXPECT_TRUE(MakeAlarmSpec(std::string(65, 'n'), 5000.0, std::nullopt, 0,
                            &spec)
                  .has_value());
  EXPECT_TRUE(
      MakeAlarmSpec("x", std::nullopt, std::nullopt, 0, &spec).has_value());
  EXPECT_TRUE(
      MakeAlarmSpec("x", std::nullopt, 29'999.0, 0, &spec).has_value());
}

TEST(AlarmScheduleTest, AOneShotFiresOnceAndIsRemoved) {
  AlarmMap alarms;
  alarms["once"] = AlarmSpec{100, std::nullopt};
  EXPECT_TRUE(CollectDueAlarms(alarms, 99).empty());
  const std::vector<AlarmFire> fires = CollectDueAlarms(alarms, 100);
  ASSERT_EQ(fires.size(), 1u);
  EXPECT_EQ(fires[0], (AlarmFire{"once", 100}));
  EXPECT_TRUE(alarms.empty());
}

// The startup catch-up: a periodic alarm missed many times fires ONCE, for
// the slot it was due at, then resumes on its own grid after now.
TEST(AlarmScheduleTest, AMissedPeriodicAlarmFiresOnceAndKeepsItsGrid) {
  AlarmMap alarms;
  alarms["hourly"] = AlarmSpec{0, 60 * kMinute};
  const double now = 5 * 60 * kMinute + 10 * kMinute;  // 5h10m later
  const std::vector<AlarmFire> fires = CollectDueAlarms(alarms, now);
  ASSERT_EQ(fires.size(), 1u);
  EXPECT_EQ(fires[0].scheduled_ms, 0);
  EXPECT_EQ(alarms["hourly"].when_ms, 6 * 60 * kMinute);
  // Exactly on a slot: the next one is a full period later.
  alarms["hourly"] = AlarmSpec{0, 60 * kMinute};
  CollectDueAlarms(alarms, 60 * kMinute);
  EXPECT_EQ(alarms["hourly"].when_ms, 120 * kMinute);
}

TEST(AlarmScheduleTest, FiresAreOrderedByScheduledTimeThenName) {
  AlarmMap alarms;
  alarms["b"] = AlarmSpec{50, std::nullopt};
  alarms["a"] = AlarmSpec{50, std::nullopt};
  alarms["early"] = AlarmSpec{10, std::nullopt};
  alarms["later"] = AlarmSpec{500, std::nullopt};
  const std::vector<AlarmFire> fires = CollectDueAlarms(alarms, 100);
  ASSERT_EQ(fires.size(), 3u);
  EXPECT_EQ(fires[0].name, "early");
  EXPECT_EQ(fires[1].name, "a");
  EXPECT_EQ(fires[2].name, "b");
  EXPECT_EQ(NextAlarmDueMs(alarms), 500);
}

TEST(AlarmScheduleTest, PrefsRoundTripAndSkipMalformedRows) {
  AlarmMap alarms;
  alarms["one"] = AlarmSpec{123, std::nullopt};
  alarms["two"] = AlarmSpec{456, kMinute};
  base::DictValue dict = AlarmsToDict(alarms);
  EXPECT_EQ(AlarmsFromDict(dict), alarms);

  base::DictValue bad;
  bad.Set("no-when", base::DictValue());
  base::DictValue short_period;
  short_period.Set("whenMs", 1.0);
  short_period.Set("periodMs", 5.0);
  bad.Set("short", std::move(short_period));
  bad.Set("not-a-dict", 3);
  EXPECT_TRUE(AlarmsFromDict(bad).empty());
  EXPECT_FALSE(NextAlarmDueMs(AlarmMap()).has_value());
}

}  // namespace
}  // namespace nexus
