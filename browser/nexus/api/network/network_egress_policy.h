// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_NETWORK_NETWORK_EGRESS_POLICY_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_NETWORK_NETWORK_EGRESS_POLICY_H_

#include <optional>
#include <string>

#include "net/base/ip_address.h"

class GURL;

namespace nexus {

// The egress policy every `network.*` request runs under: a request may
// reach the public internet and nothing else. Loopback, the private ranges
// (RFC 1918, RFC 4193), link-local (169.254/16, fe80::/10 — the cloud
// metadata endpoints live there), carrier-grade NAT (100.64/10), the
// unspecified and multicast ranges, and IPv4-mapped IPv6 forms of all of
// them are refused.
//
// Why the policy lives here and not in the JS guest: DNS resolves in the
// browser process; the guest never sees an IP, so a hostname check there is
// defeated by a name that resolves to 127.0.0.1 or rebinds between the
// check and the connect. The checks below run on the IP the socket actually
// connected to (`URLResponseHead::remote_endpoint`), at the first response
// and again at every redirect hop, plus a cheap pre-dial check on URLs whose
// host is an IP literal.
//
// Pure functions — no I/O, no threading requirements — so the unit test
// covers them without a network service.

// True when `address` may be dialed under the public-only policy.
bool IsPublicEgressAddress(const net::IPAddress& address);

// The refusal for `address`, or nullopt when it is allowed. The text names
// the range so the guest can show why.
std::optional<std::string> EgressRefusalFor(const net::IPAddress& address);

// Pre-dial check: when `url`'s host is an IP literal, the refusal for it;
// nullopt for a hostname (checked after resolution) or an allowed literal.
std::optional<std::string> EgressRefusalForLiteralHost(const GURL& url);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_NETWORK_NETWORK_EGRESS_POLICY_H_
