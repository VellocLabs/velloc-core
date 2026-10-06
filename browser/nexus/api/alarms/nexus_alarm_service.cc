// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/alarms/nexus_alarm_service.h"

#include <algorithm>
#include <cstddef>
#include <utility>

#include "base/functional/bind.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/clock.h"
#include "base/time/default_clock.h"
#include "chrome/browser/profiles/profile.h"
#include "components/keyed_service/content/browser_context_dependency_manager.h"
#include "components/prefs/pref_service.h"
#include "custom_browser/browser/nexus/api/llm/llm_pref_names.h"
#include "custom_browser/browser/nexus/launcher/nexus_launcher_delegate.h"

namespace nexus {

NexusAlarmService::NexusAlarmService(PrefService* prefs,
                                     const base::Clock* clock,
                                     base::RepeatingClosure wake)
    : prefs_(prefs), clock_(clock), wake_(std::move(wake)) {
  if (prefs_) {
    alarms_ = AlarmsFromDict(prefs_->GetDict(prefs::kNexusAlarms));
  }
  // Startup catch-up: whatever came due while the browser was not running
  // fires once now (and, with no runtime connected yet, is queued and wakes
  // one).
  CheckDue();
}

NexusAlarmService::~NexusAlarmService() = default;

double NexusAlarmService::NowMs() const {
  return clock_->Now().InMillisecondsFSinceUnixEpoch();
}

std::optional<std::string> NexusAlarmService::Create(
    const std::string& name,
    std::optional<double> when_ms,
    std::optional<double> period_ms) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  AlarmSpec spec;
  if (std::optional<std::string> error =
          MakeAlarmSpec(name, when_ms, period_ms, NowMs(), &spec)) {
    return error;
  }
  if (!alarms_.contains(name) && alarms_.size() >= kMaxAlarms) {
    return std::string("Too many alarms (256).");
  }
  alarms_[name] = spec;
  Save();
  // An alarm created in the past fires at once (through the timer, so the
  // create's reply goes out first).
  Reschedule();
  return std::nullopt;
}

bool NexusAlarmService::Clear(const std::string& name) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const bool existed = alarms_.erase(name) > 0;
  std::erase_if(pending_,
                [&name](const AlarmFire& fire) { return fire.name == name; });
  if (existed) {
    Save();
    Reschedule();
  }
  return existed;
}

std::vector<AlarmFire> NexusAlarmService::TakePending() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  std::vector<AlarmFire> out;
  out.swap(pending_);
  return out;
}

uint64_t NexusAlarmService::AddSink(Sink sink) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const uint64_t id = next_sink_id_++;
  sinks_[id] = std::move(sink);
  return id;
}

void NexusAlarmService::RemoveSink(uint64_t id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  sinks_.erase(id);
}

void NexusAlarmService::CheckDue() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  std::vector<AlarmFire> fires = CollectDueAlarms(alarms_, NowMs());
  if (!fires.empty()) {
    Save();
    Deliver(std::move(fires));
  }
  Reschedule();
}

void NexusAlarmService::Deliver(std::vector<AlarmFire> fires) {
  if (!sinks_.empty()) {
    // Snapshot: a sink may remove itself while being called.
    std::vector<Sink> sinks;
    for (const auto& [id, sink] : sinks_) {
      sinks.push_back(sink);
    }
    for (const AlarmFire& fire : fires) {
      for (const Sink& sink : sinks) {
        sink.Run(fire);
      }
    }
    return;
  }
  // No runtime: keep the fires for its `alarms.list`, and boot one.
  for (AlarmFire& fire : fires) {
    pending_.push_back(std::move(fire));
  }
  if (pending_.size() > kMaxPendingFires) {
    pending_.erase(pending_.begin(),
                   pending_.end() - static_cast<ptrdiff_t>(kMaxPendingFires));
  }
  if (wake_) {
    // Posted: this may run during profile creation, before the UI layer
    // registered its launcher delegate.
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
                                                             wake_);
  }
}

void NexusAlarmService::Save() {
  if (prefs_) {
    prefs_->SetDict(prefs::kNexusAlarms, AlarmsToDict(alarms_));
  }
}

void NexusAlarmService::Reschedule() {
  timer_.Stop();
  std::optional<double> next = NextAlarmDueMs(alarms_);
  if (!next.has_value()) {
    return;
  }
  base::TimeDelta delay = base::Milliseconds(std::max(0.0, *next - NowMs()));
  delay = std::min(delay, kMaxTimerDelay);
  timer_.Start(FROM_HERE, delay,
               base::BindOnce(&NexusAlarmService::CheckDue,
                              weak_factory_.GetWeakPtr()));
}

// static
NexusAlarmService* NexusAlarmServiceFactory::GetForProfile(Profile* profile) {
  if (!profile) {
    return nullptr;
  }
  return static_cast<NexusAlarmService*>(
      GetInstance()->GetServiceForBrowserContext(profile, /*create=*/true));
}

// static
NexusAlarmServiceFactory* NexusAlarmServiceFactory::GetInstance() {
  static base::NoDestructor<NexusAlarmServiceFactory> instance;
  return instance.get();
}

NexusAlarmServiceFactory::NexusAlarmServiceFactory()
    : BrowserContextKeyedServiceFactory(
          "NexusAlarmService",
          BrowserContextDependencyManager::GetInstance()) {}

NexusAlarmServiceFactory::~NexusAlarmServiceFactory() = default;

std::unique_ptr<KeyedService>
NexusAlarmServiceFactory::BuildServiceInstanceForBrowserContext(
    content::BrowserContext* context) const {
  Profile* profile = Profile::FromBrowserContext(context);
  if (!profile || profile->IsOffTheRecord()) {
    return nullptr;
  }
  // The service is the profile's KeyedService, destroyed before the
  // profile, so the bound pointer outlives every run of `wake`.
  return std::make_unique<NexusAlarmService>(
      profile->GetPrefs(), base::DefaultClock::GetInstance(),
      base::BindRepeating(&NexusAlarmServiceFactory::WakeRuntimeForProfile,
                          base::Unretained(profile)));
}

// static
void NexusAlarmServiceFactory::WakeRuntimeForProfile(Profile* profile) {
  // The existing way a runtime is brought up from nothing — the tray icon's
  // call — but for the profile whose alarm fired, not the last-used one: a
  // fire in profile B must boot B's runtime, which drains B's queue. Null
  // very early in startup; the fire is queued either way and drained by the
  // next runtime's list.
  if (NexusLauncherDelegate* delegate = GetNexusLauncherDelegate()) {
    delegate->OpenOrFocusNexusWindowForProfile(profile);
  }
}

bool NexusAlarmServiceFactory::ServiceIsCreatedWithBrowserContext() const {
  return true;
}

}  // namespace nexus
