// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_ALARMS_NEXUS_ALARMS_BRIDGE_ADAPTER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_ALARMS_NEXUS_ALARMS_BRIDGE_ADAPTER_H_

#include <stdint.h>

#include <vector>

#include "base/memory/weak_ptr.h"
#include "custom_browser/browser/nexus/api/alarms/alarms_api.gen.h"
#include "custom_browser/browser/nexus/bridge_response_callback.h"

namespace blink {
class MessagePortDescriptor;
}

namespace nexus {

class NexusAlarmService;
class NexusBridgeResponseDelegate;
struct AlarmFire;

// The `alarms` capability (P-3): create / clear / list over
// the per-Profile NexusAlarmService, and the `alarms.onAlarm` event. While
// its port is connected this adapter is one of the service's sinks — "a
// runtime is connected" — so fires come here as events instead of being
// queued and waking a new runtime. Between a dead port and the next one it is
// not a sink: an event sent then would be dropped, so the fire must queue.
// UI thread; every action answers synchronously.
class NexusAlarmsBridgeAdapter : public ::nexus::generated::AlarmsApiBase {
 public:
  // `service` may be null (no profile): every action then answers
  // ALARMS_UNAVAILABLE.
  NexusAlarmsBridgeAdapter(NexusBridgeResponseDelegate* delegate,
                           NexusAlarmService* service);
  ~NexusAlarmsBridgeAdapter() override;

  NexusAlarmsBridgeAdapter(const NexusAlarmsBridgeAdapter&) = delete;
  NexusAlarmsBridgeAdapter& operator=(const NexusAlarmsBridgeAdapter&) =
      delete;

  // NexusBridgeCapabilityHandler: become / stop being a sink.
  void OnBridgeConnectionOpened() override;
  void OnBridgeConnectionClosed() override;

 private:
  void HandleCreateOnUI(::nexus::generated::AlarmsCreateInput input,
                        std::vector<blink::MessagePortDescriptor> ports,
                        ::nexus::BridgeResponseCallback done) override;
  void HandleClearOnUI(::nexus::generated::AlarmsClearInput input,
                       std::vector<blink::MessagePortDescriptor> ports,
                       ::nexus::BridgeResponseCallback done) override;
  void HandleListOnUI(::nexus::generated::AlarmsListInput input,
                      std::vector<blink::MessagePortDescriptor> ports,
                      ::nexus::BridgeResponseCallback done) override;

  // The service's sink: one `alarms.onAlarm` event.
  void PushAlarmEvent(const AlarmFire& fire);

  // Weak: during profile teardown a parked panel's port service (and so
  // this adapter) can outlive the KeyedService.
  base::WeakPtr<NexusAlarmService> service_;
  // Non-zero while a sink (a connection is open).
  uint64_t sink_id_ = 0;
  base::WeakPtrFactory<NexusAlarmsBridgeAdapter> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_ALARMS_NEXUS_ALARMS_BRIDGE_ADAPTER_H_
