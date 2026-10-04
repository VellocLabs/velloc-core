// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/alarms/nexus_alarms_bridge_adapter.h"

#include <optional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/values.h"
#include "custom_browser/browser/nexus/api/alarms/nexus_alarm_service.h"

namespace nexus {

namespace {

constexpr char kUnavailableCode[] = "ALARMS_UNAVAILABLE";
constexpr char kUnavailableMessage[] =
    "alarms require an attached browser profile";

base::DictValue FireToDict(const AlarmFire& fire) {
  base::DictValue dict;
  dict.Set("name", fire.name);
  dict.Set("scheduledMs", fire.scheduled_ms);
  return dict;
}

}  // namespace

NexusAlarmsBridgeAdapter::NexusAlarmsBridgeAdapter(
    NexusBridgeResponseDelegate* delegate,
    NexusAlarmService* service)
    : ::nexus::generated::AlarmsApiBase(delegate),
      service_(service ? service->GetWeakPtr()
                       : base::WeakPtr<NexusAlarmService>()) {
  if (service_) {
    sink_id_ = service_->AddSink(base::BindRepeating(
        &NexusAlarmsBridgeAdapter::PushAlarmEvent,
        weak_factory_.GetWeakPtr()));
  }
}

NexusAlarmsBridgeAdapter::~NexusAlarmsBridgeAdapter() {
  if (service_ && sink_id_ != 0) {
    service_->RemoveSink(sink_id_);
  }
}

void NexusAlarmsBridgeAdapter::HandleCreateOnUI(
    ::nexus::generated::AlarmsCreateInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  if (!service_) {
    std::move(done).Error(kUnavailableCode, kUnavailableMessage);
    return;
  }
  if (std::optional<std::string> error =
          service_->Create(input.name, input.whenMs, input.periodMs)) {
    std::move(done).InvalidArgs(std::move(*error));
    return;
  }
  std::move(done).SuccessVoid();
}

void NexusAlarmsBridgeAdapter::HandleClearOnUI(
    ::nexus::generated::AlarmsClearInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  if (!service_) {
    std::move(done).Error(kUnavailableCode, kUnavailableMessage);
    return;
  }
  base::DictValue out;
  out.Set("cleared", service_->Clear(input.name));
  std::move(done).Success(std::move(out));
}

void NexusAlarmsBridgeAdapter::HandleListOnUI(
    ::nexus::generated::AlarmsListInput /*input*/,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  if (!service_) {
    std::move(done).Error(kUnavailableCode, kUnavailableMessage);
    return;
  }
  base::ListValue alarms;
  for (const auto& [name, spec] : service_->alarms()) {
    base::DictValue row;
    row.Set("name", name);
    row.Set("whenMs", spec.when_ms);
    if (spec.period_ms.has_value()) {
      row.Set("periodMs", *spec.period_ms);
    }
    alarms.Append(std::move(row));
  }
  base::ListValue fired;
  for (const AlarmFire& fire : service_->TakePending()) {
    fired.Append(FireToDict(fire));
  }
  base::DictValue out;
  out.Set("alarms", std::move(alarms));
  out.Set("fired", std::move(fired));
  std::move(done).Success(std::move(out));
}

void NexusAlarmsBridgeAdapter::PushAlarmEvent(const AlarmFire& fire) {
  base::DictValue route;
  route.Set("namespace", "nexus");
  route.Set("capability", "alarms");
  route.Set("action", "onAlarm");
  SendEvent(route, base::Value(FireToDict(fire)));
}

}  // namespace nexus
