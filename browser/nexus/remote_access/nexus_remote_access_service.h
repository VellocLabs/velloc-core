// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_REMOTE_ACCESS_NEXUS_REMOTE_ACCESS_SERVICE_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_REMOTE_ACCESS_NEXUS_REMOTE_ACCESS_SERVICE_H_

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/containers/flat_map.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "custom_browser/browser/nexus/remote_access/nexus_remote_access_listener.h"
#include "net/base/network_interfaces.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "url/gurl.h"

class Profile;

namespace network {
class SharedURLLoaderFactory;
}  // namespace network

namespace nexus::remote_access {

// What `Configure` / the status snapshot reports the listener is doing.
enum class ListenerState {
  kOff,         // enabled pref is false
  kStarting,    // a bind is in flight on the listener's IO thread
  kListening,   // bound and accepting
  kError,       // the config is unusable or the bind failed (error_ says why)
};

// One network interface offered to the Settings page as a bind candidate.
struct BindAddressInfo {
  std::string address;         // the IPv4 literal the listener would bind
  std::string interface_name;  // net::NetworkInterface::friendly_name

  bool operator==(const BindAddressInfo&) const = default;
};

// The full status snapshot returned by `remoteAccess.getStatus` (and pushed
// by `remoteAccess.onStatusChanged` minus the token).
struct RemoteAccessStatus {
  RemoteAccessStatus();
  ~RemoteAccessStatus();
  RemoteAccessStatus(const RemoteAccessStatus&);
  RemoteAccessStatus& operator=(const RemoteAccessStatus&);
  RemoteAccessStatus(RemoteAccessStatus&&) noexcept;
  RemoteAccessStatus& operator=(RemoteAccessStatus&&) noexcept;

  bool enabled = false;
  std::string bind_address;
  // The ACTUAL bound port when state==kListening (the OS-assigned port when
  // the configured pref was 0); otherwise the configured pref value.
  int port = 0;
  ListenerState state = ListenerState::kOff;
  std::optional<std::string> error;      // "port_in_use", "no_address", ...
  std::optional<std::string> control_url;    // ws://<bind>:<port>/host/control
  std::optional<std::string> control_token;  // memory-only, 32 bytes hex
  std::vector<BindAddressInfo> addresses;
  int clients = 0;
};

// The profile-level singleton behind the `remoteAccess` bridge capability
// (F-24). UI thread.
//
// Owns:
//   * the prefs (enabled / bind_address / port),
//   * the NexusRemoteAccessListener's start/stop,
//   * the reverse proxy's OUTBOUND fetches (SimpleURLLoader on the profile's
//     default StoragePartition — the only upstream is the guest origin),
//   * the Local-Network-Access grant the runtime needs to dial back: on
//     enable, ContentSettingsType::LOOPBACK_NETWORK = ALLOW scoped to the ONE
//     guest origin; revoked on disable (spike S1's conclusion — the grant
//     must go through HostContentSettingsMap; seeding the raw pref does NOT
//     work).
//
// The listener's IO-thread Core does the routing and WebSocket pairing and
// never touches this Profile; it reaches back here only for the proxy's
// outbound fetch, through a ProxyCallback the listener always runs on THIS
// sequence (see the threading note on NexusRemoteAccessListener).
//
// Nothing here blocks the UI thread — this object is built during browser
// startup, inside BrowserMainLoop's DisallowUnresponsiveTasks scope. Starting
// the listener is therefore asynchronous: a start moves `state` to kStarting
// and the outcome (kListening / kError) arrives as a status change.
class NexusRemoteAccessService {
 public:
  explicit NexusRemoteAccessService(Profile* profile);
  ~NexusRemoteAccessService();

  NexusRemoteAccessService(const NexusRemoteAccessService&) = delete;
  NexusRemoteAccessService& operator=(const NexusRemoteAccessService&) = delete;

  // Snapshot the current status. Never blocks the UI thread: the address
  // list is a cache (`cached_addresses_`), refreshed asynchronously by
  // RequestAddressesRefresh() on a MayBlock worker. The first snapshot after
  // construction may carry an empty/stale list until the refresh lands.
  RemoteAccessStatus GetStatus();

  // Apply a partial configure: any of the three optionals set → written to
  // the pref and the listener started/stopped to match. Returns the status as
  // of now — `kStarting` when this call began a bind; the outcome follows as
  // a status change. A configure that leaves (address, port) as they are is a
  // no-op for a listener that is already starting or listening.
  RemoteAccessStatus Configure(std::optional<bool> enabled,
                               std::optional<std::string> bind_address,
                               std::optional<int> port);

  // Test seams. Land an address refresh as if the MayBlock worker had just
  // replied (so a test can place it inside the kStarting window), and read
  // every NIC set handed to the listener's Host gate since construction.
  // Landing one also turns the real worker refresh off for the rest of the
  // service's life — GetStatus requests one on every call, and its reply
  // would race the test's set for the same window.
  void LandAddressesRefreshForTesting(std::vector<BindAddressInfo> addresses) {
    addresses_refresh_disabled_for_testing_ = true;
    OnAddressesRefreshed(std::move(addresses));
  }
  const std::vector<std::vector<std::string>>&
  local_address_pushes_for_testing() const {
    return local_address_pushes_for_testing_;
  }

  // The persisted access-link pairings (pref kNexusRemoteAccessPairings),
  // stored verbatim as a list of dicts — the runtime's pairingTable owns the
  // schema and is the only writer/reader; C++ persists the list opaquely so a
  // minted link survives a desktop restart. GetPairings returns a const view
  // of the stored list; SetPairings replaces it wholesale (the runtime always
  // writes the full table, never a delta). UI thread, no blocking I/O beyond
  // the pref store's own.
  const base::ListValue& GetPairings() const;
  // Refuses (false, nothing written) a list over kMaxStoredPairings entries
  // or holding an entry that is not a dict of at most kMaxStoredPairingBytes
  // of JSON — the list is opaque, but it is not unbounded.
  static constexpr size_t kMaxStoredPairings = 256;
  static constexpr size_t kMaxStoredPairingBytes = 4096;
  bool SetPairings(base::ListValue pairings);

  // The current status without the control token (for onStatusChanged).
  RemoteAccessStatus GetStatusForEvent();

  // Subscribe to status changes (the adapter pushes remoteAccess.onStatusChanged
  // for each): every state transition, and a changed address list. The
  // payload omits the control token.
  // Returns a subscription id for RemoveStatusSink. UI thread.
  using StatusSink = base::RepeatingCallback<void(const RemoteAccessStatus&)>;
  uint64_t AddStatusSink(StatusSink sink);
  void RemoveStatusSink(uint64_t id);

 private:
  // One in-flight reverse-proxy fetch. Concurrent requests for the same path
  // coalesce onto one upstream load.
  struct ProxyFetch {
    ProxyFetch();
    ~ProxyFetch();
    ProxyFetch(ProxyFetch&&) noexcept;
    ProxyFetch& operator=(ProxyFetch&&) noexcept;
    ProxyFetch(const ProxyFetch&) = delete;
    ProxyFetch& operator=(const ProxyFetch&) = delete;

    std::unique_ptr<network::SimpleURLLoader> loader;
    std::vector<NexusRemoteAccessListener::ProxyDoneCallback> waiters;
  };

  // Start/stop to match the prefs. Called on construction and by Configure.
  void SyncListenerToPrefs();
  void StartListener(const std::string& bind_address, int port);
  void OnListenerStarted(StartResult result, uint16_t bound_port);
  // Stops the listener and settles on `state` (kOff or kError) / `error`.
  void StopListener(ListenerState state, std::optional<std::string> error);

  // The status snapshot, token included. No side effects.
  RemoteAccessStatus BuildStatus() const;

  // The listener's ProxyCallback: fire (or join) an upstream fetch for
  // `url_path`. Runs on the UI thread.
  void HandleProxyRequest(std::string url_path,
                          std::string if_none_match,
                          std::string if_modified_since,
                          NexusRemoteAccessListener::ProxyDoneCallback done);
  void OnProxyComplete(const std::string& fetch_key,
                       std::optional<std::string> body);

  // The LNA grant for the guest origin (spike S1). Grant on enable, revoke on
  // disable. No-op when the grant is already in the requested state.
  void ApplyLoopbackGrant(bool grant);

  // Push the current status (token stripped) to every StatusSink.
  void NotifyStatusChanged();
  // The listener's /rt client count changed (owner sequence).
  void OnListenerClientsChanged(size_t count);
  // Connected /rt clients as reported: 0 unless listening.
  int ConnectedClientCount() const;

  // Compute the bindable-address list from net::GetNetworkList (IPv4
  // literals only, loopback included, 0.0.0.0 excluded). Pure + blocking:
  // runs on a MayBlock worker, never on the UI thread.
  static std::vector<BindAddressInfo> ComputeAddresses();

  // Post an async address refresh (no-op if one is already in flight). The
  // worker result updates `cached_addresses_` and pushes onStatusChanged.
  void RequestAddressesRefresh();
  void OnAddressesWorkerReply(std::vector<BindAddressInfo> addresses);
  void OnAddressesRefreshed(std::vector<BindAddressInfo> addresses);
  // Under a wildcard bind, hand the listener's Host gate the cached NIC set.
  // No-op otherwise, and while the cache is still empty.
  void PushLocalAddresses();

  const raw_ptr<Profile> profile_;
  scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory_;

  // Never null; lives as long as the service. Its IO thread exists only once
  // a start was ever asked for.
  std::unique_ptr<NexusRemoteAccessListener> listener_;
  // The (address, port) the current kStarting / kListening run was asked for.
  std::string active_bind_address_;
  int active_port_ = 0;
  // True when the active run bound the wildcard 0.0.0.0 — the control URL then
  // dials 127.0.0.1 (0.0.0.0 is not dialable) and the Host gate accepts any
  // local NIC IP.
  bool bind_ip_wildcard_ = false;
  // The 32-byte memory-only control token, minted at each start.
  std::string control_token_;
  uint16_t bound_port_ = 0;
  ListenerState state_ = ListenerState::kOff;
  std::optional<std::string> error_;
  // "path\nif-none-match\nif-modified-since" -> the in-flight fetch and its
  // coalesced waiters (same path AND same validators: a 304 is only an
  // answer for a joiner that sent the same conditional headers).
  base::flat_map<std::string, std::unique_ptr<ProxyFetch>> in_flight_proxy_;

  // Status-change subscribers (the bridge adapter), keyed by subscription id.
  std::map<uint64_t, StatusSink> status_sinks_;
  uint64_t next_status_sink_id_ = 1;

  // Last address list computed on the worker; GetStatus() copies this so it
  // never blocks the UI thread on GetNetworkList. Empty until the first
  // async refresh completes.
  std::vector<BindAddressInfo> cached_addresses_;
  // Every set PushLocalAddresses handed the listener (test observation only;
  // grows by one per NIC-set change under a wildcard bind).
  std::vector<std::vector<std::string>> local_address_pushes_for_testing_;
  bool addresses_refresh_disabled_for_testing_ = false;
  bool addresses_refresh_in_flight_ = false;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<NexusRemoteAccessService> weak_factory_{this};
};

// The per-Profile shared service (lazily created, stored as Profile
// user-data), mirroring GetSharedShellLauncher. UI-thread only.
NexusRemoteAccessService& GetRemoteAccessService(Profile* profile);

}  // namespace nexus::remote_access

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_REMOTE_ACCESS_NEXUS_REMOTE_ACCESS_SERVICE_H_
