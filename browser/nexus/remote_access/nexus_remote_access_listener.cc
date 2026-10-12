// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/remote_access/nexus_remote_access_listener.h"

#include <array>
#include <optional>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/bind_post_task.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/threading/thread.h"
#include "base/timer/timer.h"
#include "net/base/ip_address.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_errors.h"
#include "net/log/net_log_source.h"
#include "net/server/http_server.h"
#include "net/server/http_server_request_info.h"
#include "net/server/http_server_response_info.h"
#include "net/server/web_socket_encoder.h"
#include "net/socket/tcp_server_socket.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "custom_browser/browser/nexus/remote_access/remote_access_router.h"

namespace nexus::remote_access {

namespace {

// Traffic annotation for the listener's inbound connections (the reverse
// proxy responses it sends, and the /rt + /host/* WebSocket traffic). The
// OUTBOUND fetch the proxy makes carries its own annotation on the service
// side (nexus_remote_access_service.cc); this one covers what the listener
// itself puts on the wire.
constexpr net::NetworkTrafficAnnotationTag kListenerTraffic =
    net::DefineNetworkTrafficAnnotation("nexus_remote_access_listener", R"(
      semantics {
        sender: "Nexus Remote Access Listener"
        description:
          "The desktop's default-OFF embedded relay. "
          "Reverse-proxies the Nexus guest origin's immutable "
          "build entry and exposes /rt so a remote browser on the LAN/VPN "
          "can join this desktop's runtime with scope <= control."
        trigger:
          "The user explicitly enables remote access in Settings and a "
          "remote browser connects to the bound address:port."
        data:
          "HTTP request paths under the listener's own origin; WebSocket "
          "frames relayed verbatim between a remote client and the local "
          "runtime. The listener inspects only routing metadata (path, "
          "Host, Origin, token); frame payloads are forwarded unparsed."
        destination: OTHER
        internal {
          contacts {
            email: "niclas@niclas.cc"
          }
        }
        user_data {
          type: OTHER
        }
        last_reviewed: "2026-09-18"
      }
      policy {
        cookies_allowed: NO
        setting:
          "Remote access is OFF by default and toggled in Settings; the "
          "listener binds only the single user-selected address."
        policy_exception_justification:
          "Not yet covered by enterprise policy."
      }
    )");

// WebSocket pairing limits (mirrored from the dev relay).
constexpr base::TimeDelta kLegTimeout = base::Seconds(5);
constexpr size_t kMaxPreLegBufferBytes = 1 * 1024 * 1024;   // 1 MiB → 1009
// Transcript snapshots can be large; a blob chunk is 64 KiB * 4/3. Both the
// read and write buffers of every relayed socket are raised to 32 MiB so a
// big snapshot frame is not truncated by the 1 MiB default.
constexpr int32_t kRelayBufferBytes = 32 * 1024 * 1024;
// The /rt concurrency ceiling.
constexpr size_t kMaxClients = 8;
// /rt is accepted before any authentication (the runtime authenticates over
// the leg), so the ceiling alone lets one LAN host hold every slot with idle
// sockets and lock every paired device out. Cap what one remote address may
// hold; same-machine peers are the desktop's own windows and stay uncapped.
constexpr size_t kMaxClientsPerPeer = 2;
// Send-buffer limit of a connection carrying proxied HTTP responses.
constexpr int32_t kProxySendBufferBytes = 16 * 1024 * 1024;
// `GET /`'s redirect to the runtime's build entry: fresh for a year, like the
// guest origin's index.html (see HandleEntryRedirect).
constexpr char kEntryRedirectCacheControl[] = "private, max-age=31536000";

// Build the 2-byte big-endian close-code payload and wrap it in a server
// close frame (unmasked — a server never masks, RFC 6455 section 5.1).
std::string EncodeCloseFrame(int code) {
  std::string payload;
  payload.push_back(static_cast<char>((code >> 8) & 0xFF));
  payload.push_back(static_cast<char>(code & 0xFF));
  std::string frame;
  // CreateServer() = no extensions; masking_key 0 = unmasked server frame.
  net::WebSocketEncoder::CreateServer()->EncodeCloseFrame(payload, 0, &frame);
  return frame;
}

}  // namespace

ListenerConfig::ListenerConfig() = default;
ListenerConfig::~ListenerConfig() = default;
ListenerConfig::ListenerConfig(const ListenerConfig&) = default;
ListenerConfig& ListenerConfig::operator=(const ListenerConfig&) = default;
ListenerConfig::ListenerConfig(ListenerConfig&&) noexcept = default;
ListenerConfig& ListenerConfig::operator=(ListenerConfig&&) noexcept = default;

// ---- Core: the relay, IO thread only ---------------------------------------
//
// Constructed, used and destroyed on the listener's IO thread (it is held in a
// base::SequenceBound). It holds NO pointer to the handle or to anything on
// the owner's sequence: its two outbound callbacks were wrapped by the handle
// so that running them here merely posts to the owner's sequence.
class NexusRemoteAccessListener::Core : public net::HttpServer::Delegate {
 public:
  using ClientCountCallback = base::RepeatingCallback<void(size_t)>;
  // Reports an entry path learned from the runtime's control hello up to the
  // handle, which keeps it as the seed for a later restart.
  using EntryPathCallback = base::RepeatingCallback<void(std::string)>;

  Core(ProxyCallback proxy_callback,
       ClientCountCallback client_count_callback,
       EntryPathCallback entry_path_callback)
      : proxy_callback_(std::move(proxy_callback)),
        client_count_callback_(std::move(client_count_callback)),
        entry_path_callback_(std::move(entry_path_callback)) {}

  ~Core() override {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    // The browser is going away, which is NOT the user turning remote access
    // off: 4003 is final to a client (it stops reconnecting for good), so a
    // quit would strand every remote page until someone minted a new link.
    StopServer(kCloseGoingAway);
  }

  Core(const Core&) = delete;
  Core& operator=(const Core&) = delete;

  StartOutcome Start(ListenerConfig config, std::string entry_path);
  // `client_close_code` is what connected clients are closed with: 4003 only
  // when the user turned remote access OFF (a client must not come back),
  // 1001 for every other reason the server goes down (see Stop below).
  void Stop(int client_close_code);
  // The service pushes a refreshed NIC set when its off-thread address
  // refresh lands (data pushed, never pulled). Only meaningful under a
  // wildcard bind.
  void SetLocalAddresses(std::vector<std::string> local_addresses);

 private:
  // A pending /rt client whose runtime leg has not arrived yet.
  struct PendingLeg {
    int client_connection_id = 0;
    std::vector<std::string> buffered;
    size_t buffer_bytes = 0;
    std::unique_ptr<base::OneShotTimer> timer;
  };

  // net::HttpServer::Delegate:
  void OnConnect(int connection_id) override;
  void OnHttpRequest(int connection_id,
                     const net::HttpServerRequestInfo& info) override;
  void OnWebSocketRequest(int connection_id,
                          const net::HttpServerRequestInfo& info) override;
  void OnWebSocketMessage(int connection_id, std::string data) override;
  void OnClose(int connection_id) override;

  // Closes every socket (clients `client_close_code`, control 1001) and drops
  // the server.
  void StopServer(int client_close_code);

  RouteDecision Classify(const net::HttpServerRequestInfo& info,
                         bool is_websocket_upgrade) const;
  void SendReject(int connection_id, const RouteDecision& decision);

  void HandleEntryRedirect(int connection_id,
                           const net::HttpServerRequestInfo& info);
  void HandleProxyRequest(int connection_id,
                          const net::HttpServerRequestInfo& info,
                          std::string upstream_path);
  void CompleteProxyRequest(uint64_t server_epoch,
                            int connection_id,
                            int status,
                            std::map<std::string, std::string> headers,
                            std::string body);
  void AcceptClientSocket(int connection_id,
                          const net::HttpServerRequestInfo& info);
  void AcceptHostControl(int connection_id,
                         const net::HttpServerRequestInfo& info);
  void AcceptHostLeg(int connection_id,
                     const net::HttpServerRequestInfo& info,
                     std::string leg_id);
  // The 5 s leg timer fired: the runtime never claimed this leg → 4002.
  void OnLegTimeout(std::string leg_id);
  // The runtime's control socket said something. Its hello names the build
  // entry `GET /` redirects to; nothing else it could say is read.
  void HandleControlMessage(std::string_view data);

  void SendWs(int connection_id, std::string_view text);
  // Write a close frame carrying `code` then close the socket — the only way
  // to convey the protocol's close codes with net::HttpServer.
  void CloseSocketWithCode(int connection_id, int code);
  void DropControlSocket(int close_code);
  void DropAllClients(int close_code);
  // Reports client_connection_ids_.size() to the owner when it changed.
  void ReportClientCount();

  const ProxyCallback proxy_callback_;
  const ClientCountCallback client_count_callback_;
  const EntryPathCallback entry_path_callback_;

  ListenerConfig config_;
  std::unique_ptr<net::HttpServer> server_;
  // Bumped per server. Connection ids restart with each net::HttpServer, so a
  // proxy reply minted under an older server must not be sent on a new one.
  uint64_t server_epoch_ = 0;
  std::string expected_host_;      // "<bind>:<port>" after bind
  std::string entry_path_;         // last runtime-reported entry path
  // The wildcard Host gate's NIC set: Start's config copy, refreshed by the
  // service's SetLocalAddresses pushes. Read only when bind_is_wildcard.
  std::vector<std::string> local_addresses_;
  int control_connection_id_ = 0;  // the runtime's control socket (0 = none)
  // legId -> the pending client.
  std::map<std::string, PendingLeg> pending_legs_;
  // pending client connection -> its legId.
  std::map<int, std::string> leg_id_by_connection_;
  // Spliced pairs: client connection <-> leg connection.
  std::map<int, int> peer_connection_;
  // Every accepted /rt client connection id (pending AND spliced). Its size
  // is the /rt concurrency count; an id leaves the set only when that client
  // connection closes.
  std::set<int> client_connection_ids_;
  // client connection -> the remote address it came from (the per-peer cap).
  std::map<int, net::IPAddress> client_peer_address_;
  size_t reported_client_count_ = 0;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<Core> weak_factory_{this};
};

NexusRemoteAccessListener::StartOutcome NexusRemoteAccessListener::Core::Start(
    ListenerConfig config,
    std::string entry_path) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // A restart tears the previous run down first, here on the one IO sequence,
  // so rebinding the port it held cannot race its own close. Going-away, not
  // disabled: the server is coming right back, so a client must keep trying.
  StopServer(kCloseGoingAway);
  entry_path_ = std::move(entry_path);

  net::IPAddress ip;
  if (!ip.AssignFromIPLiteral(config.bind_address)) {
    LOG(WARNING) << "[RemoteAccess] bind_address is not an IP literal: "
                 << config.bind_address;
    return {StartResult::kFailed, 0};
  }

  auto socket =
      std::make_unique<net::TCPServerSocket>(nullptr, net::NetLogSource());
  const int listen_result = socket->Listen(net::IPEndPoint(ip, config.port),
                                           32, /*ipv6_only=*/std::nullopt);
  if (listen_result != net::OK) {
    LOG(WARNING) << "[RemoteAccess] Listen failed on " << config.bind_address
                 << ":" << config.port << " net_error=" << listen_result;
    switch (listen_result) {
      case net::ERR_ADDRESS_IN_USE:
        return {StartResult::kPortInUse, 0};
      case net::ERR_ADDRESS_INVALID:
      case net::ERR_ADDRESS_UNREACHABLE:
        return {StartResult::kAddressUnavailable, 0};
      default:
        return {StartResult::kFailed, 0};
    }
  }

  net::IPEndPoint local;
  if (socket->GetLocalAddress(&local) != net::OK) {
    return {StartResult::kFailed, 0};
  }

  ++server_epoch_;
  server_ = std::make_unique<net::HttpServer>(std::move(socket), this);
  expected_host_ =
      config.bind_address + ":" + base::NumberToString(local.port());
  local_addresses_ = config.local_addresses;
  config_ = std::move(config);
  LOG(INFO) << "[RemoteAccess] listening on " << expected_host_;
  return {StartResult::kOk, local.port()};
}

void NexusRemoteAccessListener::Core::Stop(int client_close_code) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  StopServer(client_close_code);
}

void NexusRemoteAccessListener::Core::SetLocalAddresses(
    std::vector<std::string> local_addresses) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  local_addresses_ = std::move(local_addresses);
}

void NexusRemoteAccessListener::Core::StopServer(int client_close_code) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!server_) {
    return;
  }
  // The control socket always gets 1001 — the runtime redials on its own.
  // Clients get 4003 (final) ONLY when remote access was switched off; every
  // other teardown says going-away so the page keeps trying and heals when
  // the desktop comes back.
  DropAllClients(client_close_code);
  DropControlSocket(kCloseGoingAway);
  server_.reset();
  // In-flight proxy replies and leg timers belong to the server that is gone.
  weak_factory_.InvalidateWeakPtrs();
  expected_host_.clear();
  config_ = ListenerConfig();
}

// ---- net::HttpServer::Delegate ---------------------------------------------

void NexusRemoteAccessListener::Core::OnConnect(int connection_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

RouteDecision NexusRemoteAccessListener::Core::Classify(
    const net::HttpServerRequestInfo& info,
    bool is_websocket_upgrade) const {
  RouteRequest request;
  request.method = info.method;
  request.raw_path = info.path;
  request.host_header = info.GetHeaderValue("host");
  request.origin_header = info.GetHeaderValue("origin");
  request.is_websocket_upgrade = is_websocket_upgrade;
  request.peer_is_loopback = info.peer.address().IsLoopback();
  // A process on this machine connecting to the machine's OWN bound interface
  // address is routed internally and presents that address (not 127.0.0.1) as
  // its peer. The runtime's host-control/leg dial is exactly that case, so
  // treat peer == our bound address as same-machine. Parse the configured
  // literal once per request (cheap, and bind_address is always an IP literal
  // here — the service rejects anything else before Start).
  net::IPAddress bound_ip;
  request.peer_is_bound_address =
      bound_ip.AssignFromIPLiteral(config_.bind_address) &&
      info.peer.address() == bound_ip;

  RouteContext context;
  context.expected_host = expected_host_;
  context.control_token = config_.control_token;
  context.interface_version = config_.interface_version;
  context.has_control_socket = control_connection_id_ != 0;
  context.relaxed_proxy = config_.relaxed_proxy;
  context.bind_is_wildcard = config_.bind_is_wildcard;
  // The live NIC set (Start's copy, refreshed by SetLocalAddresses pushes).
  context.local_addresses = local_addresses_;

  return Route(request, context);
}

void NexusRemoteAccessListener::Core::SendReject(
    int connection_id,
    const RouteDecision& decision) {
  if (decision.kind == RouteKind::kReject &&
      decision.status == RejectStatus::kForbidden) {
    server_->Send(connection_id, net::HTTP_FORBIDDEN, "forbidden\n",
                  "text/plain", kListenerTraffic);
    return;
  }
  // Everything else — including a WebSocket route requested without an
  // Upgrade, or an HTTP route requested with one — is a plain 404.
  server_->Send404(connection_id, kListenerTraffic);
}

void NexusRemoteAccessListener::Core::OnHttpRequest(
    int connection_id,
    const net::HttpServerRequestInfo& info) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const RouteDecision decision = Classify(info, /*is_websocket_upgrade=*/false);
  switch (decision.kind) {
    case RouteKind::kEntryRedirect:
      HandleEntryRedirect(connection_id, info);
      return;
    case RouteKind::kProxy:
      HandleProxyRequest(connection_id, info, decision.upstream_path);
      return;
    default:
      SendReject(connection_id, decision);
      return;
  }
}

void NexusRemoteAccessListener::Core::OnWebSocketRequest(
    int connection_id,
    const net::HttpServerRequestInfo& info) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const RouteDecision decision = Classify(info, /*is_websocket_upgrade=*/true);
  switch (decision.kind) {
    case RouteKind::kClientSocket:
      AcceptClientSocket(connection_id, info);
      return;
    case RouteKind::kHostControl:
      AcceptHostControl(connection_id, info);
      return;
    case RouteKind::kHostLeg:
      AcceptHostLeg(connection_id, info, decision.leg_id);
      return;
    default:
      SendReject(connection_id, decision);
      return;
  }
}

void NexusRemoteAccessListener::Core::OnWebSocketMessage(int connection_id,
                                                         std::string data) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // A spliced connection forwards verbatim to its peer.
  const auto peer_it = peer_connection_.find(connection_id);
  if (peer_it != peer_connection_.end()) {
    SendWs(peer_it->second, data);
    return;
  }

  // A pending /rt client whose leg has not arrived yet: buffer (cap 1 MiB).
  const auto leg_conn_it = leg_id_by_connection_.find(connection_id);
  if (leg_conn_it != leg_id_by_connection_.end()) {
    auto pending_it = pending_legs_.find(leg_conn_it->second);
    if (pending_it != pending_legs_.end()) {
      PendingLeg& pending = pending_it->second;
      if (pending.buffer_bytes + data.size() > kMaxPreLegBufferBytes) {
        const int client_id = pending.client_connection_id;
        pending_legs_.erase(pending_it);
        leg_id_by_connection_.erase(leg_conn_it);
        CloseSocketWithCode(client_id, kCloseTooBig);
        return;
      }
      pending.buffer_bytes += data.size();
      pending.buffered.push_back(std::move(data));
    }
    return;
  }

  // The control socket: the runtime's hello names the immutable build entry a
  // remote's `GET /` must land on. That is the ONLY thing it ever tells us
  // — the relay does the talking on this socket ({"open":…}/{"closed":…}).
  if (connection_id == control_connection_id_) {
    HandleControlMessage(data);
  }
}

void NexusRemoteAccessListener::Core::HandleControlMessage(
    std::string_view data) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  std::optional<std::string> reported =
      EntryPathFromHello(data, config_.interface_version);
  if (!reported) {
    // Every remote's first request is `GET /`, which answers 503 without an
    // entry path — so a runtime whose hello we cannot use is worth saying out
    // loud once, rather than leaving a bare 503 to be explained later.
    LOG(WARNING) << "nexus remote access: control message names no usable "
                    "build entry; GET / stays 503";
    return;
  }
  if (*reported == entry_path_) {
    return;  // a redial re-sending the same hello
  }
  entry_path_ = *std::move(reported);
  entry_path_callback_.Run(entry_path_);
}

// NOTE: net::HttpServer::Close() calls back into OnClose() synchronously, so
// every path below (and every caller of CloseSocketWithCode) removes a
// connection from the maps BEFORE closing it — the re-entrant call then finds
// nothing and cannot invalidate an iterator its caller still holds.
void NexusRemoteAccessListener::Core::OnClose(int connection_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // Any /rt client connection (pending or spliced) leaves the concurrency
  // set when it closes.
  if (client_connection_ids_.erase(connection_id)) {
    client_peer_address_.erase(connection_id);
    ReportClientCount();
  }

  // The control socket went away: every pending leg's client now has no
  // runtime behind it → 4001 (mirrors the dev relay).
  if (connection_id == control_connection_id_) {
    control_connection_id_ = 0;
    std::map<std::string, PendingLeg> orphaned;
    orphaned.swap(pending_legs_);
    for (auto& [leg_id, pending] : orphaned) {
      leg_id_by_connection_.erase(pending.client_connection_id);
      CloseSocketWithCode(pending.client_connection_id, kCloseRuntimeOffline);
    }
    return;
  }

  // A spliced connection closes its peer (either end's close tears the pair
  // down, mirroring the dev relay).
  const auto peer_it = peer_connection_.find(connection_id);
  if (peer_it != peer_connection_.end()) {
    const int peer_id = peer_it->second;
    peer_connection_.erase(peer_it);
    peer_connection_.erase(peer_id);
    server_->Close(peer_id);
    return;
  }

  // A pending client that closed before its leg arrived.
  const auto leg_conn_it = leg_id_by_connection_.find(connection_id);
  if (leg_conn_it != leg_id_by_connection_.end()) {
    pending_legs_.erase(leg_conn_it->second);
    leg_id_by_connection_.erase(leg_conn_it);
  }
}

// ---- Routing outcomes ------------------------------------------------------

void NexusRemoteAccessListener::Core::HandleEntryRedirect(
    int connection_id,
    const net::HttpServerRequestInfo& info) {
  if (entry_path_.empty() && config_.relaxed_proxy) {
    // Debug upstream is an unbundled vite dev server: it has no immutable
    // build entry to report, and its index IS "/". Without this the minted
    // link (http://<bind>:<port>/#rt=…) could only ever answer 503 in dev.
    HandleProxyRequest(connection_id, info, "/");
    return;
  }
  if (entry_path_.empty()) {
    server_->Send(connection_id, net::HTTP_SERVICE_UNAVAILABLE,
                  "no runtime entry path reported\n", "text/plain",
                  kListenerTraffic);
    return;
  }
  net::HttpServerResponseInfo response(net::HTTP_FOUND);
  response.AddHeader("Location", entry_path_);
  // Cacheable like the guest's /v<N>/index.html, so a remembered device opening
  // the address while this desktop is off gets its cached entry; the page
  // corrects a stale redirect itself (F-24). `private`: names this build.
  response.AddHeader("Cache-Control", kEntryRedirectCacheControl);
  server_->SendResponse(connection_id, response, kListenerTraffic);
}

void NexusRemoteAccessListener::Core::HandleProxyRequest(
    int connection_id,
    const net::HttpServerRequestInfo& info,
    std::string upstream_path) {
  // Hand the fetch to the owner. `proxy_callback_` only POSTS to the owner's
  // sequence (the handle wrapped it), and the reply posts itself back here.
  // Only the revalidation headers cross; the query never does (the router
  // already stripped it).
  proxy_callback_.Run(
      std::move(upstream_path), info.GetHeaderValue("if-none-match"),
      info.GetHeaderValue("if-modified-since"),
      base::BindPostTaskToCurrentDefault(base::BindOnce(
          &Core::CompleteProxyRequest, weak_factory_.GetWeakPtr(),
          server_epoch_, connection_id)));
}

void NexusRemoteAccessListener::Core::CompleteProxyRequest(
    uint64_t server_epoch,
    int connection_id,
    int status,
    std::map<std::string, std::string> headers,
    std::string body) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!server_ || server_epoch != server_epoch_) {
    return;
  }
  net::HttpServerResponseInfo response(
      static_cast<net::HttpStatusCode>(status));
  for (const auto& [name, value] : headers) {
    // Content-Type is set by SetBody below; adding it here would send it twice.
    if (name != "content-type" && IsForwardableResponseHeader(name)) {
      response.AddHeader(name, value);
    }
  }
  // Content-Encoding was dropped by the service (SimpleURLLoader already
  // decompressed); Content-Length is recomputed by SetBody.
  const auto ctype = headers.find("content-type");
  response.SetBody(body, ctype != headers.end()
                             ? ctype->second
                             : "application/octet-stream");
  // net::HttpServer refuses (and closes the connection on) a write above the
  // connection's send-buffer limit, 1 MiB by default — smaller than plenty of
  // bundle chunks. Keep-alive can queue several responses on one connection,
  // so allow a few maximum-size bodies (the service caps one at 5 MiB).
  server_->SetSendBufferSize(connection_id, kProxySendBufferBytes);
  // A connection that closed meanwhile is unknown to the server: a no-op.
  server_->SendResponse(connection_id, response, kListenerTraffic);
}

void NexusRemoteAccessListener::Core::AcceptClientSocket(
    int connection_id,
    const net::HttpServerRequestInfo& info) {
  server_->AcceptWebSocket(connection_id, info, kListenerTraffic);
  // No runtime behind the listener → 4001 (the "desktop panel closed" code).
  if (control_connection_id_ == 0) {
    CloseSocketWithCode(connection_id, kCloseRuntimeOffline);
    return;
  }
  // Concurrency ceiling → 4004.
  if (client_connection_ids_.size() >= kMaxClients) {
    CloseSocketWithCode(connection_id, kCloseTooManyClients);
    return;
  }
  // Per-peer ceiling → 4004 as well: one remote address may not hold the
  // whole table.
  const net::IPAddress peer_address = info.peer.address();
  if (!peer_address.IsLoopback()) {
    size_t held_by_peer = 0;
    for (const auto& [id, address] : client_peer_address_) {
      if (address == peer_address) {
        ++held_by_peer;
      }
    }
    if (held_by_peer >= kMaxClientsPerPeer) {
      CloseSocketWithCode(connection_id, kCloseTooManyClients);
      return;
    }
  }

  server_->SetReceiveBufferSize(connection_id, kRelayBufferBytes);
  server_->SetSendBufferSize(connection_id, kRelayBufferBytes);
  client_connection_ids_.insert(connection_id);
  client_peer_address_.emplace(connection_id, peer_address);
  ReportClientCount();

  // Mint an unpredictable legId (16 random bytes, hex) — one client must not
  // be able to claim another's pending leg — and tell the runtime to dial its
  // half. The client's pre-leg messages are buffered (cap 1 MiB).
  std::array<uint8_t, 16> leg_bytes;
  base::RandBytes(leg_bytes);
  const std::string leg_id = base::HexEncodeLower(leg_bytes);

  PendingLeg pending;
  pending.client_connection_id = connection_id;
  pending.timer = std::make_unique<base::OneShotTimer>();
  pending.timer->Start(FROM_HERE, kLegTimeout,
                       base::BindOnce(&Core::OnLegTimeout,
                                      weak_factory_.GetWeakPtr(), leg_id));
  pending_legs_.emplace(leg_id, std::move(pending));
  leg_id_by_connection_.emplace(connection_id, leg_id);

  SendWs(control_connection_id_, "{\"open\":\"" + leg_id + "\"}");
}

void NexusRemoteAccessListener::Core::OnLegTimeout(std::string leg_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = pending_legs_.find(leg_id);
  if (it == pending_legs_.end()) {
    return;
  }
  const int client_id = it->second.client_connection_id;
  // The timer whose task this is must outlive the task: park it past the
  // erase and let it die with this frame.
  std::unique_ptr<base::OneShotTimer> fired_timer = std::move(it->second.timer);
  pending_legs_.erase(it);
  leg_id_by_connection_.erase(client_id);
  CloseSocketWithCode(client_id, kCloseLegTimeout);
}

void NexusRemoteAccessListener::Core::AcceptHostControl(
    int connection_id,
    const net::HttpServerRequestInfo& info) {
  // Latest control socket wins: a second window's SharedWorker replaced the
  // first's. Close the previous one 1001 (going away).
  DropControlSocket(kCloseGoingAway);
  server_->AcceptWebSocket(connection_id, info, kListenerTraffic);
  server_->SetReceiveBufferSize(connection_id, kRelayBufferBytes);
  server_->SetSendBufferSize(connection_id, kRelayBufferBytes);
  control_connection_id_ = connection_id;
}

void NexusRemoteAccessListener::Core::AcceptHostLeg(
    int connection_id,
    const net::HttpServerRequestInfo& info,
    std::string leg_id) {
  server_->AcceptWebSocket(connection_id, info, kListenerTraffic);

  const auto it = pending_legs_.find(leg_id);
  if (it == pending_legs_.end()) {
    // Unknown legId — close 1000 without splicing (mirrors the dev relay).
    CloseSocketWithCode(connection_id, kCloseNormal);
    return;
  }

  PendingLeg pending = std::move(it->second);
  pending_legs_.erase(it);
  pending.timer.reset();

  server_->SetReceiveBufferSize(connection_id, kRelayBufferBytes);
  server_->SetSendBufferSize(connection_id, kRelayBufferBytes);

  const int client_id = pending.client_connection_id;
  leg_id_by_connection_.erase(client_id);

  // Flush what the client said before its runtime leg arrived, in order.
  for (const std::string& msg : pending.buffered) {
    SendWs(connection_id, msg);
  }

  // Splice the pair: verbatim forwarding both ways, paired close.
  peer_connection_.emplace(client_id, connection_id);
  peer_connection_.emplace(connection_id, client_id);

  // Informational — the control socket may already be gone.
  if (control_connection_id_ != 0) {
    SendWs(control_connection_id_, "{\"closed\":\"" + leg_id + "\"}");
  }
}

// ---- WebSocket helpers -----------------------------------------------------

void NexusRemoteAccessListener::Core::SendWs(int connection_id,
                                             std::string_view text) {
  if (!server_) {
    return;
  }
  server_->SendOverWebSocket(connection_id, text, kListenerTraffic);
}

void NexusRemoteAccessListener::Core::CloseSocketWithCode(int connection_id,
                                                          int code) {
  if (!server_) {
    return;
  }
  // net::HttpServer exposes no close-with-code; write the close frame
  // ourselves (the wire protocol's 4001/4002/4003/4004 are the WHOLE point
  // of the client's retry logic) then tear the connection down.
  server_->SendRaw(connection_id, EncodeCloseFrame(code), kListenerTraffic);
  server_->Close(connection_id);
}

void NexusRemoteAccessListener::Core::DropControlSocket(int close_code) {
  if (control_connection_id_ == 0) {
    return;
  }
  const int id = control_connection_id_;
  control_connection_id_ = 0;
  CloseSocketWithCode(id, close_code);
}

void NexusRemoteAccessListener::Core::DropAllClients(int close_code) {
  // Detach all the bookkeeping first (see the OnClose note), then close.
  std::set<int> ids;
  for (const auto& [leg_id, pending] : pending_legs_) {
    ids.insert(pending.client_connection_id);
  }
  for (const auto& [a, b] : peer_connection_) {
    ids.insert(a);
  }
  pending_legs_.clear();
  leg_id_by_connection_.clear();
  peer_connection_.clear();
  client_connection_ids_.clear();
  client_peer_address_.clear();
  for (const int id : ids) {
    CloseSocketWithCode(id, close_code);
  }
  ReportClientCount();
}

void NexusRemoteAccessListener::Core::ReportClientCount() {
  const size_t count = client_connection_ids_.size();
  if (count == reported_client_count_) {
    return;
  }
  reported_client_count_ = count;
  client_count_callback_.Run(count);
}

// ---- The handle: owner sequence --------------------------------------------

NexusRemoteAccessListener::NexusRemoteAccessListener(
    ProxyCallback proxy_callback,
    ClientsChangedCallback clients_changed)
    : proxy_callback_(std::move(proxy_callback)),
      clients_changed_(std::move(clients_changed)) {
  DCHECK(proxy_callback_);
}

NexusRemoteAccessListener::~NexusRemoteAccessListener() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!io_thread_) {
    return;
  }
  // Queue the Core's destruction on the IO thread (it closes every socket
  // with the protocol's close codes), then hand the thread to the pool to be
  // joined:
  // base::Thread::Stop() runs what is already queued before it exits, and the
  // join is a blocking wait that must never happen on the owner's (UI) thread.
  core_.Reset();
  io_thread_->DetachFromSequence();
  base::ThreadPool::PostTask(
      FROM_HERE,
      {base::MayBlock(), base::WithBaseSyncPrimitives(),
       base::TaskShutdownBehavior::BLOCK_SHUTDOWN},
      base::DoNothingWithBoundArgs(std::move(io_thread_)));
}

bool NexusRemoteAccessListener::EnsureCore() {
  if (core_) {
    return true;
  }
  auto thread = std::make_unique<base::Thread>("NexusRemoteAccessIO");
  base::Thread::Options options;
  options.message_pump_type = base::MessagePumpType::IO;
  if (!thread->StartWithOptions(std::move(options))) {
    return false;
  }
  io_thread_ = std::move(thread);
  // Both Core -> owner callbacks are wrapped HERE so the Core cannot run
  // owner-sequence code by accident: on the IO thread, Run() only posts.
  core_ = base::SequenceBound<Core>(
      io_thread_->task_runner(),
      base::BindPostTaskToCurrentDefault(proxy_callback_),
      base::BindPostTaskToCurrentDefault(base::BindRepeating(
          &NexusRemoteAccessListener::OnClientCountChanged,
          weak_factory_.GetWeakPtr())),
      base::BindPostTaskToCurrentDefault(base::BindRepeating(
          &NexusRemoteAccessListener::NoteEntryPathReported,
          weak_factory_.GetWeakPtr())));
  return true;
}

void NexusRemoteAccessListener::Start(ListenerConfig config,
                                      StartedCallback started) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const uint64_t generation = ++generation_;
  auto reply = base::BindOnce(&NexusRemoteAccessListener::OnStarted,
                              weak_factory_.GetWeakPtr(), generation,
                              std::move(started));
  if (!EnsureCore()) {
    // Same contract as a bind failure: reported asynchronously.
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(std::move(reply), StartOutcome()));
    return;
  }
  core_.AsyncCall(&Core::Start)
      .WithArgs(std::move(config), entry_path_)
      .Then(std::move(reply));
}

void NexusRemoteAccessListener::Stop(StopReason reason) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  ++generation_;  // a Start still in flight is now stale
  client_count_ = 0;
  if (core_) {
    core_.AsyncCall(&Core::Stop)
        .WithArgs(reason == StopReason::kUserDisabled ? kCloseListenerDisabled
                                                      : kCloseGoingAway);
  }
}

void NexusRemoteAccessListener::NoteEntryPathReported(std::string entry_path) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  entry_path_ = std::move(entry_path);
}

void NexusRemoteAccessListener::SetLocalAddresses(
    std::vector<std::string> local_addresses) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (core_) {
    core_.AsyncCall(&Core::SetLocalAddresses)
        .WithArgs(std::move(local_addresses));
  }
}

size_t NexusRemoteAccessListener::client_count() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return client_count_;
}

void NexusRemoteAccessListener::OnStarted(uint64_t generation,
                                          StartedCallback started,
                                          StartOutcome outcome) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (generation != generation_) {
    return;  // superseded by a later Start/Stop
  }
  std::move(started).Run(outcome.result, outcome.bound_port);
}

void NexusRemoteAccessListener::OnClientCountChanged(size_t count) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (count == client_count_) {
    return;
  }
  client_count_ = count;
  // `client_count()` alone is not enough: unless the service is told, no
  // status event carries the count and every surface keeps the 0 it read
  // when the listener came up.
  if (clients_changed_) {
    clients_changed_.Run(count);
  }
}

}  // namespace nexus::remote_access
