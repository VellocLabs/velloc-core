// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_NETWORK_NETWORK_EGRESS_POLICY_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_NETWORK_NETWORK_EGRESS_POLICY_H_

#include <optional>
#include <string>

#include "net/base/ip_address.h"

class GURL;

namespace nexus {

// Security: every `network.*` request may reach the public internet only.
// Loopback, private (RFC 1918 / 4193), link-local, carrier-grade NAT,
// unspecified and multicast ranges, and their IPv4-mapped IPv6 forms, are
// refused. Checked here because DNS resolves in the browser: on the IP
// actually connected to (first response and every redirect), pre-dial for IP
// literals, and before send via `kURLLoadOptionBlockLocalRequest`. Pure.

// True when `address` may be dialed under the public-only policy.
bool IsPublicEgressAddress(const net::IPAddress& address);

// The refusal for `address`, or nullopt when it is allowed. The text names
// the range so the guest can show why.
std::optional<std::string> EgressRefusalFor(const net::IPAddress& address);

// Pre-dial check: when `url`'s host is an IP literal, the refusal for it;
// nullopt for a hostname (checked after resolution) or an allowed literal.
std::optional<std::string> EgressRefusalForLiteralHost(const GURL& url);

// The refusal for a load that failed with `net_error`, or nullopt when the
// failure is not the egress policy's. `url` is the requested URL, named in
// the text.
std::optional<std::string> EgressRefusalForNetError(int net_error,
                                                    const GURL& url);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_NETWORK_NETWORK_EGRESS_POLICY_H_
