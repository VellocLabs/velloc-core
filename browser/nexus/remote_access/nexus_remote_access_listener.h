// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_REMOTE_ACCESS_NEXUS_REMOTE_ACCESS_LISTENER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_REMOTE_ACCESS_NEXUS_REMOTE_ACCESS_LISTENER_H_

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/threading/sequence_bound.h"

namespace base {
class Thread;
}  // namespace base

namespace nexus::remote_access {

// The WebSocket close codes the wire protocol defines (the SAME
// table the dev relay and the client's retry logic read). net::HttpServer
// exposes no close-with-code API, so the listener writes the close frame
// itself (CloseSocketWithCode) before tearing the connection down.
inline constexpr int kCloseNormal = 1000;
inline constexpr int kCloseGoingAway = 1001;
inline constexpr int kCloseProtocolError = 1002;
inline constexpr int kCloseUnsupportedData = 1003;   // binary frame, text-only
inline constexpr int kCloseTooBig = 1009;             // pre-leg buffer overrun
inline constexpr int kCloseRuntimeOffline = 4001;
inline constexpr int kCloseLegTimeout = 4002;
inline constexpr int kCloseListenerDisabled = 4003;
inline constexpr int kCloseTooManyClients = 4004;

// What a start attempt produced. `kPortInUse` is a FIRST-CLASS state (never
// silently switch ports — the minted link would die), and
// `kAddressUnavailable` is how "this machine no longer has that address" is
// learned: the OS refuses the bind, so nobody has to enumerate interfaces (a
// blocking call) on the UI thread to pre-validate it.
enum class StartResult { kOk, kPortInUse, kAddressUnavailable, kFailed };

// Everything one listening run is configured by.
struct ListenerConfig {
  ListenerConfig();
  ~ListenerConfig();
  ListenerConfig(const ListenerConfig&);
  ListenerConfig& operator=(const ListenerConfig&);
  ListenerConfig(ListenerConfig&&) noexcept;
  ListenerConfig& operator=(ListenerConfig&&) noexcept;

  std::string bind_address;  // an IP literal (0.0.0.0 = wildcard, accepted)
  uint16_t port = 0;         // 0 = OS-assigned ephemeral
  int interface_version = 0;
  std::string control_token;
  bool relaxed_proxy = false;
  // Set when bind_address is the wildcard 0.0.0.0: the Host gate then matches
  // the request's Host against local_addresses + the bound port (a client can
  // arrive on any NIC). Pushed by the service; the Core never enumerates NICs.
  bool bind_is_wildcard = false;
  // The machine's own IPv4 literals at Start time (incl. loopback). Only read
  // when bind_is_wildcard. Data is pushed, never pulled (threading rules).
  std::vector<std::string> local_addresses;
};

// The embedded relay. Threading: the handle lives on the owner's (UI)
// sequence; its Core (net::HttpServer, routing, pairing state) lives only on a
// dedicated IO thread, and the two only post tasks. Nothing blocks;
// `ProxyCallback` always runs on the owner's sequence; the Core has no Profile.
class NexusRemoteAccessListener {
 public:
  // proxy_callback(url_path, if_none_match, if_modified_since, done) asks the
  // owner to fetch `url_path` from the guest origin. Runs on the owner's
  // sequence. `done` receives (http_status, filtered_headers, body) and may be
  // run from any sequence (it posts itself back to the IO thread); dropping
  // it unrun is safe.
  using ProxyDoneCallback =
      base::OnceCallback<void(int status,
                              std::map<std::string, std::string> headers,
                              std::string body)>;
  using ProxyCallback =
      base::RepeatingCallback<void(std::string url_path,
                                   std::string if_none_match,
                                   std::string if_modified_since,
                                   ProxyDoneCallback done)>;
  // (result, bound_port). `bound_port` is the actual port on kOk (== the
  // configured port unless that was 0), else 0.
  using StartedCallback = base::OnceCallback<void(StartResult, uint16_t)>;
  // Run on the owner's sequence whenever the Core reports a different number
  // of connected /rt clients. `Stop` resets the count to 0 WITHOUT running it:
  // the caller of Stop owns that state change and reports it itself.
  using ClientsChangedCallback = base::RepeatingCallback<void(size_t)>;

  explicit NexusRemoteAccessListener(
      ProxyCallback proxy_callback,
      ClientsChangedCallback clients_changed = ClientsChangedCallback());
  ~NexusRemoteAccessListener();

  NexusRemoteAccessListener(const NexusRemoteAccessListener&) = delete;
  NexusRemoteAccessListener& operator=(const NexusRemoteAccessListener&) = delete;

  // Start listening per `config`; `started` is run asynchronously on this
  // sequence with the outcome. If a previous run is still up it is stopped
  // FIRST, on the same IO sequence — so restarting onto the port the previous
  // run held cannot collide with itself. A Start/Stop issued before `started`
  // ran supersedes it: the stale callback is dropped, never run.
  void Start(ListenerConfig config, StartedCallback started);

  // Why the listener is going down. It decides the close code connected
  // clients see, and that decides whether they ever come back: 4003
  // (`listener_disabled`) is FINAL on the client — it stops reconnecting for
  // good — so it belongs to the one case where coming back would be wrong.
  enum class StopReason {
    // The user switched remote access off. The link must stay dead until they
    // switch it on again.
    kUserDisabled,
    // Anything else: the browser is quitting, the config is being re-bound,
    // the bind failed. The desktop may be back in a second, so clients are
    // told going-away (1001) and keep retrying.
    kGoingAway,
  };

  // Stop listening: every connected client is closed per `reason`, the control
  // socket with 1001. Returns immediately. Safe when not started.
  void Stop(StopReason reason);

  // Push a refreshed set of the machine's own IPv4 literals (the wildcard
  // Host gate matches the request's Host against these). The service calls
  // this when its off-thread address refresh lands under a wildcard bind, so
  // the Core never enumerates NICs on its IO thread. Owner sequence.
  void SetLocalAddresses(std::vector<std::string> local_addresses);

  // Currently-connected /rt clients, as last reported by the Core
  // (eventually consistent; owner sequence).
  size_t client_count() const;

 private:
  class Core;
  struct StartOutcome {
    StartResult result = StartResult::kFailed;
    uint16_t bound_port = 0;
  };

  // Lazily brings up the IO thread + Core. False when the thread won't start.
  bool EnsureCore();
  void OnStarted(uint64_t generation,
                 StartedCallback started,
                 StartOutcome outcome);
  void OnClientCountChanged(size_t count);
  // The Core read an entry path out of the runtime's control hello. Recorded
  // here so a LATER Start (a restart: the user changed the port, or toggled
  // remote access off and on) seeds the fresh Core with it instead of
  // answering `GET /` with 503 until the runtime's control socket redials.
  // Never pushed back down — the Core is where it came from.
  void NoteEntryPathReported(std::string entry_path);

  const ProxyCallback proxy_callback_;
  const ClientsChangedCallback clients_changed_;
  // The entry path last reported by the runtime ("/v<N>/b/<key>/"), or empty
  // when none has been: the seed for the next Start.
  std::string entry_path_;
  size_t client_count_ = 0;
  // Bumped by every Start/Stop; a Start reply carrying an older value is stale.
  uint64_t generation_ = 0;

  std::unique_ptr<base::Thread> io_thread_;
  base::SequenceBound<Core> core_;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<NexusRemoteAccessListener> weak_factory_{this};
};

}  // namespace nexus::remote_access

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_REMOTE_ACCESS_NEXUS_REMOTE_ACCESS_LISTENER_H_
