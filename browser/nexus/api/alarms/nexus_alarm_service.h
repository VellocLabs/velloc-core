// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_ALARMS_NEXUS_ALARM_SERVICE_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_ALARMS_NEXUS_ALARM_SERVICE_H_

#include <stdint.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/sequence_checker.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "components/keyed_service/content/browser_context_keyed_service_factory.h"
#include "components/keyed_service/core/keyed_service.h"
#include "custom_browser/browser/nexus/api/alarms/alarm_schedule.h"

class PrefService;
class Profile;

namespace base {
class Clock;
}

namespace content {
class BrowserContext;
}

namespace nexus {

// The `alarms` capability's clock (P-3): named wall-clock alarms that survive
// a closed page and a browser restart. Per Profile; the `nexus.alarms` pref is
// the source of truth. Alarms already due at creation fire once; a fire with
// no connected sink is queued (`TakePending`) and the Nexus window is opened.
// The timer is re-armed at most an hour ahead. UI thread only.
class NexusAlarmService : public KeyedService {
 public:
  using Sink = base::RepeatingCallback<void(const AlarmFire&)>;

  static constexpr size_t kMaxPendingFires = 256;
  static constexpr base::TimeDelta kMaxTimerDelay = base::Hours(1);

  // `wake` boots a runtime when a fire finds none (the factory binds it to
  // WakeRuntimeForProfile with the service's own profile). `clock` is the wall
  // clock (a test's mock clock).
  NexusAlarmService(PrefService* prefs,
                    const base::Clock* clock,
                    base::RepeatingClosure wake);
  ~NexusAlarmService() override;

  NexusAlarmService(const NexusAlarmService&) = delete;
  NexusAlarmService& operator=(const NexusAlarmService&) = delete;

  // Creates or replaces `name`. The error message, or nullopt.
  std::optional<std::string> Create(const std::string& name,
                                    std::optional<double> when_ms,
                                    std::optional<double> period_ms);
  // True when `name` existed.
  bool Clear(const std::string& name);
  const AlarmMap& alarms() const { return alarms_; }

  // Fires that found no connected runtime, oldest first; cleared by the call.
  std::vector<AlarmFire> TakePending();

  // A connected bridge. Returns an id for RemoveSink.
  uint64_t AddSink(Sink sink);
  void RemoveSink(uint64_t id);

  // Bridge adapters hold the service weakly: a parked panel's port service
  // can outlive this KeyedService during profile teardown.
  base::WeakPtr<NexusAlarmService> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

  // Runs the due check now (tests; the timer calls it too).
  void CheckDueForTesting() { CheckDue(); }

 private:
  void CheckDue();
  void Deliver(std::vector<AlarmFire> fires);
  void Save();
  void Reschedule();
  double NowMs() const;

  raw_ptr<PrefService> prefs_;
  raw_ptr<const base::Clock> clock_;
  base::RepeatingClosure wake_;
  AlarmMap alarms_;
  std::vector<AlarmFire> pending_;
  std::map<uint64_t, Sink> sinks_;
  uint64_t next_sink_id_ = 1;
  base::OneShotTimer timer_;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<NexusAlarmService> weak_factory_{this};
};

class NexusAlarmServiceFactory : public BrowserContextKeyedServiceFactory {
 public:
  // Null for a null or off-the-record profile.
  static NexusAlarmService* GetForProfile(Profile* profile);
  static NexusAlarmServiceFactory* GetInstance();

  // A fire found no runtime: open or focus `profile`'s Nexus window (the
  // service's `wake`). Public for tests.
  static void WakeRuntimeForProfile(Profile* profile);

  NexusAlarmServiceFactory(const NexusAlarmServiceFactory&) = delete;
  NexusAlarmServiceFactory& operator=(const NexusAlarmServiceFactory&) =
      delete;

 private:
  friend class base::NoDestructor<NexusAlarmServiceFactory>;

  NexusAlarmServiceFactory();
  ~NexusAlarmServiceFactory() override;

  // BrowserContextKeyedServiceFactory:
  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override;
  // Created with the profile: missed alarms are caught up at startup, not
  // when the Nexus UI first opens.
  bool ServiceIsCreatedWithBrowserContext() const override;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_ALARMS_NEXUS_ALARM_SERVICE_H_
