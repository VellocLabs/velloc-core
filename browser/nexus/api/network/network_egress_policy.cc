// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/network/network_egress_policy.h"

#include <string>

#include "base/strings/stringprintf.h"
#include "net/base/ip_address.h"
#include "net/base/net_errors.h"
#include "net/base/url_util.h"
#include "url/gurl.h"

namespace nexus {

namespace {

// 100.64.0.0/10 — carrier-grade NAT (RFC 6598). Not a "private" range in
// net::IPAddress's vocabulary, but never a public host either.
bool IsCarrierGradeNat(const net::IPAddress& v4) {
  const auto bytes = v4.bytes();
  return bytes[0] == 100 && (bytes[1] & 0xC0) == 64;
}

// 0.0.0.0/8 — "this network". Some stacks route it to loopback.
bool IsThisNetwork(const net::IPAddress& v4) {
  return v4.bytes()[0] == 0;
}

// 169.254.0.0/16 — IPv4 link-local, where cloud metadata services answer
// (169.254.169.254 on AWS / GCP / Azure).
bool IsV4LinkLocal(const net::IPAddress& v4) {
  const auto bytes = v4.bytes();
  return bytes[0] == 169 && bytes[1] == 254;
}

// 224.0.0.0/4 multicast and 240.0.0.0/4 reserved (incl. 255.255.255.255).
bool IsV4MulticastOrReserved(const net::IPAddress& v4) {
  return (v4.bytes()[0] & 0xF0) >= 0xE0;
}

std::optional<std::string> RefusalForV4(const net::IPAddress& v4) {
  if (v4.IsLoopback()) {
    return "loopback address";
  }
  if (IsThisNetwork(v4)) {
    return "unspecified (0.0.0.0/8) address";
  }
  if (IsV4LinkLocal(v4)) {
    return "link-local (169.254/16) address";
  }
  if (IsCarrierGradeNat(v4)) {
    return "carrier-grade NAT (100.64/10) address";
  }
  if (IsV4MulticastOrReserved(v4)) {
    return "multicast or reserved address";
  }
  // RFC 1918: 10/8, 172.16/12, 192.168/16. `IsPubliclyRoutable` also
  // refuses a few more reserved blocks (192.0.0.0/24, 198.18/15, ...);
  // all of them are non-public, so one answer covers them.
  if (!v4.IsPubliclyRoutable()) {
    return "private network address";
  }
  return std::nullopt;
}

std::optional<std::string> RefusalForV6(const net::IPAddress& v6) {
  if (v6.IsIPv4MappedIPv6()) {
    // ::ffff:a.b.c.d — judge the embedded IPv4.
    return RefusalForV4(net::ConvertIPv4MappedIPv6ToIPv4(v6));
  }
  if (v6.IsLoopback() || v6.IsZero()) {
    return v6.IsZero() ? "unspecified (::) address" : "loopback address";
  }
  if (v6.IsLinkLocal()) {
    return "link-local (fe80::/10) address";
  }
  if (v6.IsUniqueLocalIPv6()) {
    return "unique-local (fc00::/7) address";
  }
  const auto bytes = v6.bytes();
  if (bytes[0] == 0xFF) {
    return "multicast address";
  }
  if (!v6.IsPubliclyRoutable()) {
    return "private network address";
  }
  return std::nullopt;
}

}  // namespace

std::optional<std::string> EgressRefusalFor(const net::IPAddress& address) {
  if (!address.IsValid()) {
    // No endpoint to judge (a cached response, a synthetic reply): nothing
    // was dialed, so nothing to refuse.
    return std::nullopt;
  }
  if (address.IsIPv4()) {
    return RefusalForV4(address);
  }
  if (address.IsIPv6()) {
    return RefusalForV6(address);
  }
  return "address of an unknown family";
}

bool IsPublicEgressAddress(const net::IPAddress& address) {
  return !EgressRefusalFor(address).has_value();
}

std::optional<std::string> EgressRefusalForLiteralHost(const GURL& url) {
  if (!url.is_valid() || !url.HostIsIPAddress()) {
    return std::nullopt;
  }
  net::IPAddress literal;
  // `HostIsIPAddress` is true for bracketed IPv6 too; strip the brackets.
  if (!literal.AssignFromIPLiteral(url.HostNoBrackets())) {
    return "unparseable IP literal host";
  }
  std::optional<std::string> refusal = EgressRefusalFor(literal);
  if (!refusal) {
    return std::nullopt;
  }
  return base::StringPrintf("host %s is a %s", std::string(url.host()).c_str(),
                            refusal->c_str());
}

std::optional<std::string> EgressRefusalForNetError(int net_error,
                                                    const GURL& url) {
  if (net_error != net::ERR_BLOCKED_BY_LOCAL_NETWORK_ACCESS_CHECKS) {
    return std::nullopt;
  }
  return base::StringPrintf(
      "%s resolved to a loopback or private network address; the "
      "connection was refused before the request was sent",
      std::string(url.host()).c_str());
}

}  // namespace nexus
