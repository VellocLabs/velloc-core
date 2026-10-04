// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/alarms/alarm_schedule.h"

#include <algorithm>
#include <cmath>
#include <tuple>
#include <utility>

namespace nexus {

namespace {

constexpr char kWhenKey[] = "whenMs";
constexpr char kPeriodKey[] = "periodMs";

}  // namespace

std::optional<std::string> MakeAlarmSpec(const std::string& name,
                                         std::optional<double> when_ms,
                                         std::optional<double> period_ms,
                                         double now_ms,
                                         AlarmSpec* out) {
  if (name.empty() || name.size() > kMaxAlarmNameChars) {
    return "An alarm name is 1 to 64 characters.";
  }
  if (period_ms.has_value() &&
      (!std::isfinite(*period_ms) || *period_ms < kMinAlarmPeriodMs)) {
    return "periodMs must be at least 30000.";
  }
  if (when_ms.has_value() && !std::isfinite(*when_ms)) {
    return "whenMs must be a finite time.";
  }
  if (!when_ms.has_value() && !period_ms.has_value()) {
    return "An alarm needs whenMs, periodMs or both.";
  }
  out->period_ms = period_ms;
  out->when_ms = when_ms.has_value() ? *when_ms : now_ms + *period_ms;
  return std::nullopt;
}

std::vector<AlarmFire> CollectDueAlarms(AlarmMap& alarms, double now_ms) {
  std::vector<AlarmFire> fires;
  for (auto it = alarms.begin(); it != alarms.end();) {
    AlarmSpec& spec = it->second;
    if (spec.when_ms > now_ms) {
      ++it;
      continue;
    }
    fires.push_back(AlarmFire{it->first, spec.when_ms});
    if (!spec.period_ms.has_value()) {
      it = alarms.erase(it);
      continue;
    }
    // The first slot strictly after now on the original grid.
    const double period = *spec.period_ms;
    const double missed = std::floor((now_ms - spec.when_ms) / period) + 1;
    spec.when_ms += missed * period;
    ++it;
  }
  std::ranges::sort(fires, [](const AlarmFire& a, const AlarmFire& b) {
    return std::tie(a.scheduled_ms, a.name) < std::tie(b.scheduled_ms, b.name);
  });
  return fires;
}

std::optional<double> NextAlarmDueMs(const AlarmMap& alarms) {
  std::optional<double> next;
  for (const auto& [name, spec] : alarms) {
    if (!next.has_value() || spec.when_ms < *next) {
      next = spec.when_ms;
    }
  }
  return next;
}

AlarmMap AlarmsFromDict(const base::DictValue& dict) {
  AlarmMap alarms;
  for (const auto [name, value] : dict) {
    const base::DictValue* row = value.GetIfDict();
    if (!row) {
      continue;
    }
    std::optional<double> when = row->FindDouble(kWhenKey);
    if (!when.has_value() || !std::isfinite(*when)) {
      continue;
    }
    AlarmSpec spec;
    spec.when_ms = *when;
    std::optional<double> period = row->FindDouble(kPeriodKey);
    if (period.has_value()) {
      if (!std::isfinite(*period) || *period < kMinAlarmPeriodMs) {
        continue;
      }
      spec.period_ms = period;
    }
    alarms.emplace(name, spec);
  }
  return alarms;
}

base::DictValue AlarmsToDict(const AlarmMap& alarms) {
  base::DictValue dict;
  for (const auto& [name, spec] : alarms) {
    base::DictValue row;
    row.Set(kWhenKey, spec.when_ms);
    if (spec.period_ms.has_value()) {
      row.Set(kPeriodKey, *spec.period_ms);
    }
    dict.Set(name, std::move(row));
  }
  return dict;
}

}  // namespace nexus
