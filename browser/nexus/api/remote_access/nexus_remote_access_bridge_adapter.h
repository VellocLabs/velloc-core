// Copyright 2026 VellocLabs
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

// Concrete adapter implementing the generated `RemoteAccessApiBase` virtuals —
// status + configure for the desktop's embedded relay (F-24), forwarding to
// the per-Profile NexusRemoteAccessService on UI. Pushes
// `remoteAccess.onStatusChanged` (getStatus minus the control token).
// Security: only the runtime calls it; the page port can reach it too, which
// is acceptable as the local trusted desktop guest. (P-3)
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
