// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_ALARMS_ALARM_SCHEDULE_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_ALARMS_ALARM_SCHEDULE_H_

#include <stddef.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "base/values.h"

namespace nexus {

// The `alarms` capability's schedule, as pure functions over a map — no
// clock, no timer, no prefs — so the rules are pinned by the lite tests.
// Times are wall-clock milliseconds since the Unix epoch (the guest's
// Date.now()).

struct AlarmSpec {
  // Next time it is due.
  double when_ms = 0;
  // Repeat interval; absent for a one-shot alarm.
  std::optional<double> period_ms;

  bool operator==(const AlarmSpec&) const = default;
};

using AlarmMap = std::map<std::string, AlarmSpec>;

// One fire: the alarm's name and the time it was scheduled for (not the
// time it ran — a missed alarm reports when it should have fired).
struct AlarmFire {
  std::string name;
  double scheduled_ms = 0;

  bool operator==(const AlarmFire&) const = default;
};

inline constexpr double kMinAlarmPeriodMs = 30'000;
inline constexpr size_t kMaxAlarms = 256;
inline constexpr size_t kMaxAlarmNameChars = 64;

// Validates and normalizes a create request at `now_ms`: a missing `when`
// with a period means "one period from now"; neither is an error, as are a
// period under kMinAlarmPeriodMs, a non-finite time and a name outside 1-64
// chars. Returns the error message, or nullopt with `out` filled.
std::optional<std::string> MakeAlarmSpec(const std::string& name,
                                         std::optional<double> when_ms,
                                         std::optional<double> period_ms,
                                         double now_ms,
                                         AlarmSpec* out);

// Every alarm due at `now_ms` fires ONCE, however many periods were missed
// (a browser that was off for a day does not fire a daily alarm 24 times
// for an hourly one). A periodic alarm then moves to its first slot after
// `now_ms` on its original grid; a one-shot alarm is removed. Fires are
// ordered by scheduled time, then name.
std::vector<AlarmFire> CollectDueAlarms(AlarmMap& alarms, double now_ms);

// The earliest `when_ms`, if any alarm exists.
std::optional<double> NextAlarmDueMs(const AlarmMap& alarms);

// The prefs form: {name: {whenMs, periodMs?}}. Malformed rows are skipped.
AlarmMap AlarmsFromDict(const base::DictValue& dict);
base::DictValue AlarmsToDict(const AlarmMap& alarms);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_ALARMS_ALARM_SCHEDULE_H_
