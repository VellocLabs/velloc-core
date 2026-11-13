// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/network/nexus_proxy_endpoint.h"

#include "base/strings/escape.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "net/base/proxy_string_util.h"

namespace nexus {

namespace {

constexpr std::string_view kSchemeSeparator = "://";

bool IsKnownScheme(std::string_view scheme) {
  return scheme == "http" || scheme == "https" || scheme == "socks4" ||
         scheme == "socks5";
}

}  // namespace

ProxyEndpoint::ProxyEndpoint() = default;
ProxyEndpoint::ProxyEndpoint(const ProxyEndpoint&) = default;
ProxyEndpoint& ProxyEndpoint::operator=(const ProxyEndpoint&) = default;
ProxyEndpoint::~ProxyEndpoint() = default;

const char* ProxyEndpointProblemWireName(ProxyEndpointProblem problem) {
  switch (problem) {
    case ProxyEndpointProblem::kMalformed:
      return "malformed";
    case ProxyEndpointProblem::kScheme:
      return "scheme";
    case ProxyEndpointProblem::kCredentials:
      return "credentials";
  }
  return "malformed";
}

base::expected<ProxyEndpoint, ProxyEndpointProblem> ParseProxyEndpoint(
    std::string_view text,
    ProxyCredentials credentials) {
  std::string_view rest = base::TrimWhitespaceASCII(text, base::TRIM_ALL);
  const size_t separator = rest.find(kSchemeSeparator);
  if (separator == std::string_view::npos || separator == 0) {
    return base::unexpected(ProxyEndpointProblem::kMalformed);
  }
  const std::string scheme = base::ToLowerASCII(rest.substr(0, separator));
  if (!IsKnownScheme(scheme)) {
    return base::unexpected(ProxyEndpointProblem::kScheme);
  }
  rest.remove_prefix(separator + kSchemeSeparator.size());
  if (rest.ends_with('/')) {
    rest.remove_suffix(1);
  }
  if (rest.empty() ||
      rest.find_first_of("/?# \t\r\n") != std::string_view::npos) {
    return base::unexpected(ProxyEndpointProblem::kMalformed);
  }

  ProxyEndpoint endpoint;
  const size_t at = rest.rfind('@');
  if (at != std::string_view::npos) {
    const bool socks = scheme == "socks4" || scheme == "socks5";
    if (credentials == ProxyCredentials::kRefuse || socks) {
      return base::unexpected(ProxyEndpointProblem::kCredentials);
    }
    const std::string_view userinfo = rest.substr(0, at);
    const size_t colon = userinfo.find(':');
    endpoint.username =
        base::UnescapeBinaryURLComponent(userinfo.substr(0, colon));
    if (colon != std::string_view::npos) {
      endpoint.password =
          base::UnescapeBinaryURLComponent(userinfo.substr(colon + 1));
    }
    if (endpoint.username.empty()) {
      return base::unexpected(ProxyEndpointProblem::kMalformed);
    }
    rest.remove_prefix(at + 1);
  }

  endpoint.server = net::ProxyUriToProxyServer(
      base::StrCat({scheme, kSchemeSeparator, rest}),
      net::ProxyServer::SCHEME_HTTP);
  if (!endpoint.server.is_valid() || endpoint.server.GetHost().empty()) {
    return base::unexpected(ProxyEndpointProblem::kMalformed);
  }
  return endpoint;
}

}  // namespace nexus
