// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_REMOTE_ACCESS_REMOTE_ACCESS_ROUTER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_REMOTE_ACCESS_REMOTE_ACCESS_ROUTER_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nexus::remote_access {

// How one inbound request is to be answered. Pure classification: the
// router decides WHAT a request is, the listener decides what to do about
// it. No sockets, no threads, no Profile — which is why the whole routing
// table is unit-testable.
enum class RouteKind {
  // Reverse-proxy this path to the guest origin. `upstream_path` carries
  // the (normalized, query-less) path to request.
  kProxy,
  // 302 to the runtime's reported entry path, or 503 when there is none.
  kEntryRedirect,
  // The remote client's WebSocket entry.
  kClientSocket,
  // The runtime's control socket.
  kHostControl,
  // The runtime's half of one client; `leg_id` is populated.
  kHostLeg,
  // Reject. `status` says with what.
  kReject,
};

// Why a request was rejected, mapped to the status the listener sends.
// 403 for a Host/Origin mismatch (an attacker's page or a DNS rebind),
// 404 for everything else INCLUDING an unauthenticated /host/* — the
// existence of those routes is not for unauthenticated callers to learn
// (mirrored from the dev relay's own comment).
enum class RejectStatus {
  kNotFound,     // 404
  kForbidden,    // 403
};

struct RouteDecision {
  RouteDecision();
  ~RouteDecision();
  RouteDecision(const RouteDecision&);
  RouteDecision& operator=(const RouteDecision&);
  RouteDecision(RouteDecision&&) noexcept;
  RouteDecision& operator=(RouteDecision&&) noexcept;

  RouteKind kind = RouteKind::kReject;
  RejectStatus status = RejectStatus::kNotFound;
  // kProxy: the path to fetch from the guest origin. Never carries a
  // query (the proxy forwards no query).
  std::string upstream_path;
  // kHostLeg: the 32-hex leg id.
  std::string leg_id;
};

// The inbound request, reduced to what routing depends on.
struct RouteRequest {
  RouteRequest();
  ~RouteRequest();
  RouteRequest(const RouteRequest&);
  RouteRequest& operator=(const RouteRequest&);
  RouteRequest(RouteRequest&&) noexcept;
  RouteRequest& operator=(RouteRequest&&) noexcept;

  std::string method;
  // Request target as it arrived, e.g. "/v1/assets/x.js?y=1".
  std::string raw_path;
  // `Host` header verbatim.
  std::string host_header;
  // `Origin` header verbatim (empty when absent).
  std::string origin_header;
  bool is_websocket_upgrade = false;
  // Whether the peer's address is loopback. /host/* is loopback-only.
  bool peer_is_loopback = false;
  // Whether the peer's address IS the listener's own bound interface address.
  // A process connecting to the machine's own LAN IP is routed internally and
  // presents that LAN IP (not 127.0.0.1) as its peer — the runtime dialing
  // its own listener from the same machine is exactly this. Same-machine by
  // definition, so it passes the /host/* gate exactly like loopback.
  bool peer_is_bound_address = false;
};

// What the listener knows that routing depends on.
struct RouteContext {
  RouteContext();
  ~RouteContext();
  RouteContext(const RouteContext&);
  RouteContext& operator=(const RouteContext&);
  RouteContext(RouteContext&&) noexcept;
  RouteContext& operator=(RouteContext&&) noexcept;

  // "<bind_address>:<port>" — the ONLY acceptable Host header when the bind
  // is a single literal. Under a wildcard bind (bind_is_wildcard) this is
  // "0.0.0.0:<port>" and the wildcard rule below applies instead.
  std::string expected_host;
  // The listener's control token; compared in constant time.
  std::string control_token;
  // The interface version this binary pins, e.g. 1.
  int interface_version = 0;
  // True when a runtime control socket is attached.
  bool has_control_socket = false;
  // Debug builds relax the proxy to all GETs so an unbundled vite dev
  // server works as the upstream.
  bool relaxed_proxy = false;
  // True when the listener bound the wildcard 0.0.0.0 (the JS layer's default
  // C++ is a pure executor and merely accepts the literal).
  // A wildcard listener must answer clients that arrive on ANY of the
  // machine's NICs, so the Host gate matches the request's Host against
  // local_addresses + the bound port instead of the single literal.
  bool bind_is_wildcard = false;
  // The machine's own IPv4 literals (net::GetNetworkList + loopback), pushed
  // by the service. Only consulted when bind_is_wildcard: a remote's
  // "Host: <nic-ip>:<port>" matches iff <nic-ip> is one of these and the port
  // is the bound one. DNS-rebinding protection is preserved — an arbitrary
  // hostname or a foreign IP still 403s.
  std::vector<std::string> local_addresses;
};

// Classify one request: the whole routing table (F-24).
RouteDecision Route(const RouteRequest& request, const RouteContext& context);

// The Host/Origin gate shared by Route's two checks. With a single-literal
// bind, `host_header` must equal context.expected_host exactly. With a
// wildcard bind (0.0.0.0), the client addressed one of the machine's real
// NICs, so the header's <ip> must be one of context.local_addresses and its
// port the bound port — anything else is still a DNS rebind / stray page.
bool HostMatches(const std::string& host_header, const RouteContext& context);

// --- pieces exposed for testing / reuse -----------------------------------

// Split "/a/b?c=d" into its path. Percent-decodes ONLY to detect traversal;
// the returned path is the decoded, normalized form. Returns false when the
// path escapes its root or is otherwise unsafe: any `..` segment (literal or
// percent-encoded, e.g. `%2e%2e`), a backslash (Windows separator), a NUL,
// or a malformed escape. Duplicate slashes are collapsed.
bool NormalizePath(std::string_view raw_path, std::string* out_path);

// Strip the query from a request target.
std::string_view PathOfTarget(std::string_view raw_path);

// Read one query parameter out of a request target.
std::string QueryParam(std::string_view raw_path, std::string_view name);

// Constant-time string comparison for the control token.
bool ConstantTimeEquals(std::string_view a, std::string_view b);

// True when `path` is one of the three proxyable shapes for this interface
// version: the immutable build entry and its two files, a
// versioned asset, or one of the fixed files — each matched exactly, under
// /v<N>/ or at the root depending on where the deploy serves it.
bool IsProxyablePath(std::string_view path, int interface_version);

// True when `path` is EXACTLY one build's immutable entry directory for this
// interface version — `/v<N>/b/<key>/` and nothing appended. The strict half
// of IsProxyablePath (which also admits the entry's index.html and
// assets-manifest.json), and the ONLY shape the listener will put in a
// `Location:` header: that is what makes a path the runtime reported over its
// control socket safe to redirect to — it can name no other origin, carry no
// CR/LF, and hold no traversal.
bool IsBuildEntryPath(std::string_view path, int interface_version);

// The runtime's first control-socket message — `{"hello":1,"entryPath":…}`
// (F-24) — reduced to the one thing the listener keeps
// from it: the immutable build entry a remote's `GET /` is redirected to.
// nullopt means "nothing usable here", and the listener keeps whatever it had:
// a non-JSON frame, a frame with no `hello` key, an absent or null `entryPath`
// (a dev runtime has no versioned entry to name), or a path that is not this
// interface version's exact build entry — including one for a DIFFERENT
// version, which this listener's proxy would 404 anyway.
std::optional<std::string> EntryPathFromHello(std::string_view message,
                                              int interface_version);

// Response headers the proxy is allowed to pass through, lowercased.
// Content-Encoding is DROPPED (SimpleURLLoader already decompressed, so
// the header would describe bytes we are not sending) and Content-Length
// is recomputed; Set-Cookie / HSTS / Alt-Svc and hop-by-hop headers are
// dropped outright.
bool IsForwardableResponseHeader(std::string_view lowercase_name);

// Request headers forwarded upstream: revalidation only.
bool IsForwardableRequestHeader(std::string_view lowercase_name);

}  // namespace nexus::remote_access

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_REMOTE_ACCESS_REMOTE_ACCESS_ROUTER_H_
