// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/remote_access/nexus_remote_access_bridge_adapter.h"

#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/values.h"
#include "custom_browser/browser/nexus/remote_access/nexus_remote_access_service.h"

namespace nexus {

namespace {

using base::Value;
using Dict = base::DictValue;
using remote_access::ListenerState;
using remote_access::NexusRemoteAccessService;
using remote_access::RemoteAccessStatus;

constexpr char kRemoteAccessUnavailableCode[] = "REMOTE_ACCESS_UNAVAILABLE";
constexpr char kRemoteAccessUnavailableMessage[] =
    "remote access requires an attached browser profile";

const char* StateToString(ListenerState state) {
  switch (state) {
    case ListenerState::kOff:
      return "off";
    case ListenerState::kStarting:
      return "starting";
    case ListenerState::kListening:
      return "listening";
    case ListenerState::kError:
      return "error";
  }
  return "off";
}

// Render the service's status snapshot as the bridge result dict. Mirrors
// the TS-side RemoteAccessStatus shape (protocol/remoteAccess.ts). The
// caller decides whether the control token is included: getStatus includes
// it (full-trust), onStatusChanged strips it.
Dict StatusToDict(const RemoteAccessStatus& status) {
  Dict dict;
  dict.Set("enabled", status.enabled);
  dict.Set("bindAddress", status.bind_address);
  dict.Set("port", status.port);
  dict.Set("state", StateToString(status.state));
  if (status.error.has_value()) {
    dict.Set("error", *status.error);
  }
  if (status.control_url.has_value()) {
    dict.Set("controlUrl", *status.control_url);
  }
  if (status.control_token.has_value()) {
    dict.Set("controlToken", *status.control_token);
  }
  base::ListValue addresses;
  for (const auto& a : status.addresses) {
    Dict entry;
    entry.Set("address", a.address);
    entry.Set("interfaceName", a.interface_name);
    addresses.Append(std::move(entry));
  }
  dict.Set("addresses", std::move(addresses));
  dict.Set("clients", status.clients);
  return dict;
}

}  // namespace

NexusRemoteAccessBridgeAdapter::NexusRemoteAccessBridgeAdapter(
    NexusBridgeResponseDelegate* delegate,
    Profile* profile)
    : ::nexus::generated::RemoteAccessApiBase(delegate), profile_(profile) {
  // Push onStatusChanged for the lifetime of this adapter. The service is
  // Profile-owned and outlives this adapter, so the sink is weak-guarded and
  // removed in the destructor.
  if (profile_) {
    status_sink_id_ =
        remote_access::GetRemoteAccessService(profile_).AddStatusSink(
            base::BindRepeating(
                &NexusRemoteAccessBridgeAdapter::PushStatusChangedEvent,
                weak_factory_.GetWeakPtr()));
  }
}

NexusRemoteAccessBridgeAdapter::~NexusRemoteAccessBridgeAdapter() {
  if (profile_ && status_sink_id_ != 0) {
    remote_access::GetRemoteAccessService(profile_)
        .RemoveStatusSink(status_sink_id_);
  }
}

void NexusRemoteAccessBridgeAdapter::PushStatusChangedEvent(
    const RemoteAccessStatus& status) {
  Dict route;
  route.Set("namespace", "nexus");
  route.Set("capability", "remoteAccess");
  route.Set("action", "onStatusChanged");
  // The service's GetStatusForEvent already stripped the token.
  SendEvent(route, base::Value(StatusToDict(status)));
}

void NexusRemoteAccessBridgeAdapter::HandleGetStatusOnUI(
    ::nexus::generated::RemoteAccessGetStatusInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  if (!profile_) {
    std::move(done).Error(kRemoteAccessUnavailableCode,
                          kRemoteAccessUnavailableMessage);
    return;
  }
  NexusRemoteAccessService& service =
      remote_access::GetRemoteAccessService(profile_);
  std::move(done).Success(StatusToDict(service.GetStatus()));
}

void NexusRemoteAccessBridgeAdapter::HandleConfigureOnUI(
    ::nexus::generated::RemoteAccessConfigureInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  if (!profile_) {
    std::move(done).Error(kRemoteAccessUnavailableCode,
                          kRemoteAccessUnavailableMessage);
    return;
  }
  NexusRemoteAccessService& service =
      remote_access::GetRemoteAccessService(profile_);
  // Absent keys stay unchanged (the codegen delivers them as std::optional).
  const RemoteAccessStatus status = service.Configure(
      input.enabled, input.bindAddress,
      input.port.has_value()
          ? std::optional<int>(static_cast<int>(*input.port))
          : std::nullopt);
  std::move(done).Success(StatusToDict(status));
}

void NexusRemoteAccessBridgeAdapter::HandleGetPairingsOnUI(
    ::nexus::generated::RemoteAccessGetPairingsInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  if (!profile_) {
    std::move(done).Error(kRemoteAccessUnavailableCode,
                          kRemoteAccessUnavailableMessage);
    return;
  }
  NexusRemoteAccessService& service =
      remote_access::GetRemoteAccessService(profile_);
  // Mirror the stored list into the result. The pairSecret rides this full-
  // trust surface (like the control token in getStatus); it is never pushed
  // into an event or a topic.
  Dict result;
  base::ListValue pairings;
  for (const base::Value& p : service.GetPairings()) {
    pairings.Append(p.Clone());
  }
  result.Set("pairings", std::move(pairings));
  std::move(done).Success(std::move(result));
}

void NexusRemoteAccessBridgeAdapter::HandleSetPairingsOnUI(
    ::nexus::generated::RemoteAccessSetPairingsInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  if (!profile_) {
    std::move(done).Error(kRemoteAccessUnavailableCode,
                          kRemoteAccessUnavailableMessage);
    return;
  }
  NexusRemoteAccessService& service =
      remote_access::GetRemoteAccessService(profile_);
  // The runtime always writes the FULL table (no delta); store it verbatim.
  base::ListValue list;
  for (base::DictValue& p : input.pairings) {
    list.Append(std::move(p));
  }
  if (!service.SetPairings(std::move(list))) {
    std::move(done).Error("REMOTE_ACCESS_PAIRINGS_TOO_LARGE",
                          "pairings list exceeds the stored-size limits");
    return;
  }
  Dict result;
  result.Set("ok", true);
  std::move(done).Success(std::move(result));
}

}  // namespace nexus
