// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/remote_access/remote_access_router.h"

#include <optional>
#include <string>
#include <string_view>

#include "base/json/json_reader.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/values.h"

namespace nexus::remote_access {

namespace {

// The fixed files a remote page needs beyond the entry and assets, exact
// matches under /v<N>/ (their root forms 404 upstream). The manifest and its
// two icons make the remote page installable (F-24).
constexpr const char* kFixedVersionedFiles[] = {
    "product_logo_128.png",
    "velloc.icon",
    "manifest.webmanifest",
    "app_icon_192.png",
    "app_icon_512.png",
};
// Root: the page's one root-relative fetch (binaryRegistry.ts's
// MANAGED_BINARIES_PATH), staged at the site root by stage_deploy.mjs as
// version-independent pin data.
constexpr const char* kFixedRootFiles[] = {
    "/managed-binaries.json",
};

// Hex digit value; -1 for anything that is not 0-9 / a-f / A-F.
constexpr int HexValue(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

// Decode one '%' escape. Returns the byte, or -1 when the escape is malformed
// (not followed by two hex digits). `i` points at '%'.
int DecodeEscape(std::string_view s, size_t i) {
  if (i + 2 >= s.size()) {
    return -1;
  }
  const int hi = HexValue(s[i + 1]);
  const int lo = HexValue(s[i + 2]);
  if (hi < 0 || lo < 0) {
    return -1;
  }
  return (hi << 4) | lo;
}

bool IsHex32(std::string_view s) {
  if (s.size() != 32) {
    return false;
  }
  for (char c : s) {
    if (HexValue(c) < 0) {
      return false;
    }
  }
  return true;
}

// A build key is the immutable `nexus-build-time` rendered as digits + 'T' +
// 'Z'. Enforcing the alphabet here keeps the route from proxying
// a made-up key.
bool IsBuildKey(std::string_view s) {
  if (s.empty()) {
    return false;
  }
  for (char c : s) {
    const bool ok = (c >= '0' && c <= '9') || c == 'T' || c == 'Z';
    if (!ok) {
      return false;
    }
  }
  return true;
}

// Join the version prefix, e.g. 1 → "/v1/".
std::string VersionPrefix(int version) {
  return "/v" + base::NumberToString(version) + "/";
}

}  // namespace

RouteDecision::RouteDecision() = default;
RouteDecision::~RouteDecision() = default;
RouteDecision::RouteDecision(const RouteDecision&) = default;
RouteDecision& RouteDecision::operator=(const RouteDecision&) = default;
RouteDecision::RouteDecision(RouteDecision&&) noexcept = default;
RouteDecision& RouteDecision::operator=(RouteDecision&&) noexcept = default;

RouteRequest::RouteRequest() = default;
RouteRequest::~RouteRequest() = default;
RouteRequest::RouteRequest(const RouteRequest&) = default;
RouteRequest& RouteRequest::operator=(const RouteRequest&) = default;
RouteRequest::RouteRequest(RouteRequest&&) noexcept = default;
RouteRequest& RouteRequest::operator=(RouteRequest&&) noexcept = default;

RouteContext::RouteContext() = default;
RouteContext::~RouteContext() = default;
RouteContext::RouteContext(const RouteContext&) = default;
RouteContext& RouteContext::operator=(const RouteContext&) = default;
RouteContext::RouteContext(RouteContext&&) noexcept = default;
RouteContext& RouteContext::operator=(RouteContext&&) noexcept = default;

std::string_view PathOfTarget(std::string_view raw_path) {
  const size_t q = raw_path.find('?');
  if (q == std::string_view::npos) {
    return raw_path;
  }
  return raw_path.substr(0, q);
}

std::string QueryParam(std::string_view raw_path, std::string_view name) {
  const size_t q = raw_path.find('?');
  if (q == std::string_view::npos) {
    return std::string();
  }
  const std::string_view query = raw_path.substr(q + 1);
  size_t pos = 0;
  while (pos <= query.size()) {
    const size_t amp = query.find('&', pos);
    const std::string_view pair =
        query.substr(pos, amp == std::string_view::npos ? std::string_view::npos
                                                        : amp - pos);
    const size_t eq = pair.find('=');
    const std::string_view key =
        pair.substr(0, eq == std::string_view::npos ? pair.size() : eq);
    if (key == name) {
      const std::string_view value =
          eq == std::string_view::npos ? std::string_view() : pair.substr(eq + 1);
      // The token is hex; percent-decoding the query is unnecessary for it,
      // so compare raw.
      return std::string(value);
    }
    if (amp == std::string_view::npos) {
      break;
    }
    pos = amp + 1;
  }
  return std::string();
}

bool ConstantTimeEquals(std::string_view a, std::string_view b) {
  // XOR-accumulate over the longer length so the loop count leaks only the
  // length difference, not where the first difference is.
  const size_t n = a.size() > b.size() ? a.size() : b.size();
  unsigned char diff = a.size() == b.size() ? 0 : 1;
  for (size_t i = 0; i < n; ++i) {
    const unsigned char x =
        i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
    const unsigned char y =
        i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
    diff |= static_cast<unsigned char>(x ^ y);
  }
  return diff == 0;
}

bool NormalizePath(std::string_view raw_path, std::string* out_path) {
  const std::string_view no_query = PathOfTarget(raw_path);
  std::string decoded;
  decoded.reserve(no_query.size());
  for (size_t i = 0; i < no_query.size(); ++i) {
    const char c = no_query[i];
    if (c == '%') {
      const int byte = DecodeEscape(no_query, i);
      if (byte < 0) {
        return false;
      }
      decoded.push_back(static_cast<char>(byte));
      i += 2;
    } else if (c == '\\') {
      // A backslash is a separator on Windows; treat as a bypass attempt.
      return false;
    } else if (c == '\0') {
      return false;
    } else {
      decoded.push_back(c);
    }
  }

  // Collapse duplicate slashes, then split into segments to reject traversal.
  std::string collapsed;
  collapsed.reserve(decoded.size());
  bool prev_slash = false;
  for (const char c : decoded) {
    if (c == '/') {
      if (prev_slash) {
        continue;  // "//" → "/"
      }
      prev_slash = true;
    } else {
      prev_slash = false;
    }
    collapsed.push_back(c);
  }

  // Segment-level traversal check. After decoding, a literal ".." segment is
  // a bypass; a segment that merely CONTAINS dots (e.g. ".well-known",
  // "..foo", "x..") is a normal file name and allowed.
  std::string out;
  out.reserve(collapsed.size());
  size_t pos = 0;
  while (pos <= collapsed.size()) {
    const size_t slash = collapsed.find('/', pos);
    const std::string_view seg =
        collapsed.empty()
            ? std::string_view()
            : std::string_view(collapsed).substr(
                  pos,
                  slash == std::string::npos ? std::string_view::npos
                                             : slash - pos);
    if (seg == "..") {
      return false;
    }
    // Rebuild, preserving separators. Skip the empty segment before a leading
    // '/' and the empty segment of a trailing '/'.
    if (!seg.empty() || pos == 0) {
      out.append(seg);
    }
    if (slash == std::string::npos) {
      break;
    }
    out.push_back('/');
    pos = slash + 1;
  }

  *out_path = std::move(out);
  return true;
}

bool IsProxyablePath(std::string_view path, int interface_version) {
  const std::string prefix = VersionPrefix(interface_version);
  // Immutable entry: /v<N>/b/<key>/, optionally + index.html /
  // assets-manifest.json. Everything else under /b/ is not served.
  const std::string build_prefix = prefix + "b/";
  if (base::StartsWith(path, build_prefix,
                       base::CompareCase::SENSITIVE)) {
    const std::string_view rest =
        std::string_view(path).substr(build_prefix.size());
    const size_t slash = rest.find('/');
    if (slash == std::string_view::npos) {
      return false;  // "/b/<key>" without a trailing slash is not an entry.
    }
    const std::string_view key = rest.substr(0, slash);
    if (!IsBuildKey(key)) {
      return false;
    }
    const std::string_view tail = rest.substr(slash);  // includes leading '/'
    return tail == "/" || tail == "/index.html" ||
           tail == "/assets-manifest.json";
  }
  // Versioned assets: /v<N>/assets/**
  const std::string assets_prefix = prefix + "assets/";
  if (base::StartsWith(path, assets_prefix, base::CompareCase::SENSITIVE)) {
    // Traversal was already rejected by NormalizePath; require something after
    // the prefix.
    return path.size() > assets_prefix.size();
  }
  for (const char* fixed : kFixedVersionedFiles) {
    if (path == prefix + fixed) {
      return true;
    }
  }
  for (const char* fixed : kFixedRootFiles) {
    if (path == fixed) {
      return true;
    }
  }
  return false;
}

bool IsBuildEntryPath(std::string_view path, int interface_version) {
  const std::string build_prefix = VersionPrefix(interface_version) + "b/";
  if (!base::StartsWith(path, build_prefix, base::CompareCase::SENSITIVE)) {
    return false;
  }
  const std::string_view rest = path.substr(build_prefix.size());
  // "<key>/" and nothing else. A non-empty key plus its trailing slash is at
  // least two characters, and anything after that slash (index.html, an
  // injected header, a second segment) leaves `back()` something other than
  // '/' or pushes a '/' into the key, which IsBuildKey then refuses.
  if (rest.size() < 2 || rest.back() != '/') {
    return false;
  }
  return IsBuildKey(rest.substr(0, rest.size() - 1));
}

std::optional<std::string> EntryPathFromHello(std::string_view message,
                                              int interface_version) {
  // JSON_PARSE_RFC: strict, like every other parse of untrusted text in the
  // tree — no comments, no trailing commas, no raw control characters.
  const std::optional<base::DictValue> hello =
      base::JSONReader::ReadDict(message, base::JSON_PARSE_RFC);
  // Only the hello carries an entry path; `{"closed":…}` and anything a
  // future protocol revision adds must not be read as one.
  if (!hello || !hello->contains("hello")) {
    return std::nullopt;
  }
  // FindString also answers null for a JSON null — which is exactly what a dev
  // runtime sends (no versioned entry to name).
  const std::string* entry_path = hello->FindString("entryPath");
  if (!entry_path || !IsBuildEntryPath(*entry_path, interface_version)) {
    return std::nullopt;
  }
  return *entry_path;
}

bool IsForwardableResponseHeader(std::string_view n) {
  return n == "content-type" || n == "cache-control" || n == "etag" ||
         n == "last-modified";
}

bool IsForwardableRequestHeader(std::string_view n) {
  return n == "if-none-match" || n == "if-modified-since";
}

bool HostMatches(const std::string& host_header, const RouteContext& context) {
  if (!context.bind_is_wildcard) {
    return host_header == context.expected_host;
  }
  // Wildcard bind (0.0.0.0:<port>): expected_host is "0.0.0.0:<port>", but a
  // remote client addressed a real NIC. Parse the header as "<host>:<port>"
  // and require the host to be one of the machine's own IPv4 literals and the
  // port to be the bound one. Loopback is in local_addresses (the runtime
  // dials 127.0.0.1), so a same-machine control leg passes here too.
  const size_t colon = host_header.rfind(':');
  if (colon == std::string::npos) {
    return false;
  }
  const std::string host = host_header.substr(0, colon);
  const std::string port = host_header.substr(colon + 1);
  // The bound port is the tail of expected_host ("0.0.0.0:<port>").
  const size_t exp_colon = context.expected_host.rfind(':');
  if (exp_colon == std::string::npos) {
    return false;
  }
  if (port != context.expected_host.substr(exp_colon + 1)) {
    return false;
  }
  for (const std::string& local : context.local_addresses) {
    if (host == local) {
      return true;
    }
  }
  return false;
}

RouteDecision Route(const RouteRequest& request, const RouteContext& context) {
  RouteDecision decision;

  // Host must match the bound address:port — exactly for a single-literal
  // bind, or one of the machine's NIC literals + the bound port under a
  // 0.0.0.0 wildcard. Anything else is a DNS rebind or a stray page — 403.
  const bool host_ok = HostMatches(request.host_header, context);

  // The control token arrives as ?token=; derive it here (raw — the token
  // is hex, so no decoding is needed to compare it).
  const std::string token = QueryParam(request.raw_path, "token");

  // /host/* answers BEFORE the Host gate: the runtime is on the same machine
  // but its stack is free to send any Host (often "127.0.0.1:<port>" rather
  // than the bound LAN address). These routes are protected by the loopback
  // + token gate instead, and unauthenticated probes get a flat 404.
  std::string path;
  if (!NormalizePath(request.raw_path, &path)) {
    decision.kind = RouteKind::kReject;
    decision.status = RejectStatus::kNotFound;
    return decision;
  }

  // /host/* is same-machine-only. Same-machine = loopback peer OR a peer
  // whose address equals the listener's own bound interface address: a process
  // connecting to the machine's own LAN IP is routed internally and presents
  // that LAN IP as its peer, so the runtime dialing its own listener arrives
  // as (LAN-IP, not 127.0.0.1) and must be accepted. A FOREIGN machine's
  // address never equals our own bound interface address, so the gate is
  // unchanged for anything off-box.
  const bool same_machine =
      request.peer_is_loopback || request.peer_is_bound_address;
  if (path == "/host/control") {
    if (!same_machine ||
        !ConstantTimeEquals(token, context.control_token)) {
      decision.kind = RouteKind::kReject;
      decision.status = RejectStatus::kNotFound;
      return decision;
    }
    decision.kind = RouteKind::kHostControl;
    return decision;
  }
  constexpr std::string_view kLegPrefix = "/host/leg/";
  if (base::StartsWith(path, kLegPrefix, base::CompareCase::SENSITIVE)) {
    const std::string_view leg_id = std::string_view(path).substr(kLegPrefix.size());
    if (!same_machine ||
        !ConstantTimeEquals(token, context.control_token) ||
        !IsHex32(leg_id)) {
      decision.kind = RouteKind::kReject;
      decision.status = RejectStatus::kNotFound;
      return decision;
    }
    decision.kind = RouteKind::kHostLeg;
    decision.leg_id = std::string(leg_id);
    return decision;
  }

  // From here the Host gate applies.
  if (!host_ok) {
    decision.kind = RouteKind::kReject;
    decision.status = RejectStatus::kForbidden;
    return decision;
  }

  if (request.method != "GET") {
    decision.kind = RouteKind::kReject;
    decision.status = RejectStatus::kNotFound;
    return decision;
  }

  if (path == "/rt") {
    if (!request.is_websocket_upgrade) {
      decision.kind = RouteKind::kReject;
      decision.status = RejectStatus::kNotFound;
      return decision;
    }
    // Security: the Origin must be the listener's own http origin (under a
    // wildcard bind, gated by the same Host rule); a scheme-less or non-http
    // Origin is refused, so a cross-origin LAN page cannot drive the runtime.
    constexpr std::string_view kHttpScheme = "http://";
    if (!base::StartsWith(request.origin_header, kHttpScheme,
                          base::CompareCase::SENSITIVE)) {
      decision.kind = RouteKind::kReject;
      decision.status = RejectStatus::kForbidden;
      return decision;
    }
    const std::string_view origin_host =
        std::string_view(request.origin_header).substr(kHttpScheme.size());
    if (!HostMatches(std::string(origin_host), context)) {
      decision.kind = RouteKind::kReject;
      decision.status = RejectStatus::kForbidden;
      return decision;
    }
    decision.kind = RouteKind::kClientSocket;
    return decision;
  }

  if (path == "/") {
    decision.kind = RouteKind::kEntryRedirect;
    return decision;
  }

  if (context.relaxed_proxy) {
    // Debug upstream is an unbundled vite dev server; let every GET through
    // (HMR's websocket is NOT proxied — it never reaches here as a GET route).
    // The query crosses too, in THIS mode only: a dev server's module graph is
    // addressed by it (`?import`, `?v=<hash>`, `?t=<stamp>` select a different
    // response for the same path), so stripping it breaks the page. Release
    // never reaches here and never forwards a query.
    decision.kind = RouteKind::kProxy;
    decision.upstream_path = path;
    const size_t q = request.raw_path.find('?');
    if (q != std::string::npos) {
      decision.upstream_path += request.raw_path.substr(q);
    }
    return decision;
  }

  if (IsProxyablePath(path, context.interface_version)) {
    decision.kind = RouteKind::kProxy;
    decision.upstream_path = path;
    return decision;
  }

  decision.kind = RouteKind::kReject;
  decision.status = RejectStatus::kNotFound;
  return decision;
}

}  // namespace nexus::remote_access
