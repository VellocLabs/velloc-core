// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_REMOTE_ACCESS_NEXUS_REMOTE_ACCESS_BRIDGE_ADAPTER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_REMOTE_ACCESS_NEXUS_REMOTE_ACCESS_BRIDGE_ADAPTER_H_

#include <cstdint>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "custom_browser/browser/nexus/api/remote_access/remote_access_api.gen.h"
#include "custom_browser/browser/nexus/bridge_response_callback.h"
#include "custom_browser/browser/nexus/remote_access/nexus_remote_access_service.h"

class Profile;

namespace blink {
class MessagePortDescriptor;
}

namespace nexus {

class NexusBridgeResponseDelegate;

// Concrete adapter implementing the generated `RemoteAccessApiBase` virtuals
// — the guest-facing status + configure surface for the desktop's embedded
// relay (remote-browser-client plan §8.4).
//
//   remoteAccess.getStatus  → NexusRemoteAccessService::GetStatus
//   remoteAccess.configure  → NexusRemoteAccessService::Configure
//
// A pure forwarder onto the per-Profile shared NexusRemoteAccessService
// (GetRemoteAccessService), so every bridge port drives ONE listener. Both
// actions run on UI (the *.api.json declare runtime.thread = "ui"); the
// service is sequence-bound to UI and synchronous, so no thread hop.
//
// The adapter ALSO subscribes to the service's status changes for its
// lifetime and pushes `remoteAccess.onStatusChanged` (payload = getStatus's
// result MINUS the control token — §8.8) so the runtime dials / disconnects
// its relay dialer reactively instead of polling.
//
// Only the runtime calls this (P-3: host calls originate from the runtime,
// never a client); the page port could technically reach it too (C++ cannot
// tell the two kGuest ports apart), which is acceptable — the page is the
// local trusted desktop guest.
class NexusRemoteAccessBridgeAdapter
    : public ::nexus::generated::RemoteAccessApiBase {
 public:
  NexusRemoteAccessBridgeAdapter(NexusBridgeResponseDelegate* delegate,
                                 Profile* profile);
  ~NexusRemoteAccessBridgeAdapter() override;

  NexusRemoteAccessBridgeAdapter(const NexusRemoteAccessBridgeAdapter&) =
      delete;
  NexusRemoteAccessBridgeAdapter& operator=(
      const NexusRemoteAccessBridgeAdapter&) = delete;

 private:
  void HandleGetStatusOnUI(
      ::nexus::generated::RemoteAccessGetStatusInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleConfigureOnUI(
      ::nexus::generated::RemoteAccessConfigureInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleGetPairingsOnUI(
      ::nexus::generated::RemoteAccessGetPairingsInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleSetPairingsOnUI(
      ::nexus::generated::RemoteAccessSetPairingsInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  // Status-sink callback: push onStatusChanged (token stripped by the
  // service). Weak-guarded so a teardown racing a transition drops cleanly.
  void PushStatusChangedEvent(
      const remote_access::RemoteAccessStatus& status);

  const raw_ptr<Profile> profile_;
  // Subscription id from AddStatusSink; 0 = not registered.
  uint64_t status_sink_id_ = 0;

  base::WeakPtrFactory<NexusRemoteAccessBridgeAdapter> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_REMOTE_ACCESS_NEXUS_REMOTE_ACCESS_BRIDGE_ADAPTER_H_
