// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_NETWORK_NEXUS_PROXY_ENDPOINT_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_NETWORK_NEXUS_PROXY_ENDPOINT_H_

#include <string>
#include <string_view>

#include "base/types/expected.h"
#include "net/base/proxy_server.h"

namespace nexus {

// One proxy server a person or a provider named, as `scheme://host[:port]`.
struct ProxyEndpoint {
  ProxyEndpoint();
  ProxyEndpoint(const ProxyEndpoint&);
  ProxyEndpoint& operator=(const ProxyEndpoint&);
  ~ProxyEndpoint();

  net::ProxyServer server;
  // Empty unless the text carried `user:password@` and the caller allowed it.
  std::string username;
  std::string password;
};

// Why a proxy text was refused. The wire names travel to the guest as
// `error.data.reason`.
enum class ProxyEndpointProblem {
  kMalformed,    // "malformed": not scheme://host[:port].
  kScheme,       // "scheme": not http, https, socks4 or socks5.
  kCredentials,  // "credentials": a username or password where none may be.
};

const char* ProxyEndpointProblemWireName(ProxyEndpointProblem problem);

enum class ProxyCredentials {
  kRefuse,
  // HTTP and HTTPS proxies only: the SOCKS client offers no authentication,
  // so a SOCKS proxy with credentials is refused either way.
  kAllowForHttp,
};

// Contract: parses `text` (surrounding whitespace ignored, one trailing `/`
// allowed). The scheme is required. A missing port is the scheme's default.
// Percent-escapes in the username and password are decoded. Pure.
base::expected<ProxyEndpoint, ProxyEndpointProblem> ParseProxyEndpoint(
    std::string_view text,
    ProxyCredentials credentials);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_NETWORK_NEXUS_PROXY_ENDPOINT_H_
