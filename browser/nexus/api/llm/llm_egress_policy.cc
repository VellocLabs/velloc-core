// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/llm/llm_egress_policy.h"

#include <utility>

#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "net/base/host_port_pair.h"
#include "net/base/proxy_string_util.h"

namespace nexus {

namespace {

base::unexpected<EgressRefusal> Refuse(const char* reason) {
  EgressRefusal refusal;
  refusal.reason = reason;
  return base::unexpected(std::move(refusal));
}

bool HostIsListed(const net::ProxyServer& server,
                  const base::ListValue& hosts) {
  const std::string bracketed = server.GetHost();
  const std::string& bare = server.host_port_pair().host();
  for (const base::Value& entry : hosts) {
    if (!entry.is_string()) {
      continue;
    }
    const std::string& host = entry.GetString();
    if (!host.empty() && (base::EqualsCaseInsensitiveASCII(host, bracketed) ||
                          base::EqualsCaseInsensitiveASCII(host, bare))) {
      return true;
    }
  }
  return false;
}

}  // namespace

EgressRefusal::EgressRefusal() = default;
EgressRefusal::EgressRefusal(EgressRefusal&&) = default;
EgressRefusal& EgressRefusal::operator=(EgressRefusal&&) = default;
EgressRefusal::~EgressRefusal() = default;

base::expected<ProxyEndpoint, EgressRefusal> ResolveEgressEndpoint(
    const base::DictValue& egress,
    const SecretLookup& lookup) {
  const std::string* url = egress.FindString("url");
  const base::ListValue* hosts = egress.FindList("hosts");
  if (!url || url->empty() || !hosts || hosts->empty()) {
    return Refuse("malformed");
  }
  std::string expanded;
  if (std::optional<SecretTemplateError> error =
          ExpandSecretTemplates(*url, lookup, &expanded)) {
    EgressRefusal refusal;
    refusal.secret = std::move(error);
    return base::unexpected(std::move(refusal));
  }
  auto endpoint =
      ParseProxyEndpoint(expanded, ProxyCredentials::kAllowForHttp);
  if (!endpoint.has_value()) {
    return Refuse(ProxyEndpointProblemWireName(endpoint.error()));
  }
  if (endpoint->server.scheme() == net::ProxyServer::SCHEME_SOCKS4) {
    return Refuse("scheme");
  }
  if (!HostIsListed(endpoint->server, *hosts)) {
    return Refuse("host");
  }
  return std::move(*endpoint);
}

std::string EgressContextKey(const ProxyEndpoint& endpoint) {
  // A newline cannot occur in a parsed proxy URI, so the parts cannot run
  // into each other.
  return base::StrCat({net::ProxyServerToProxyUri(endpoint.server), "\n",
                       endpoint.username, "\n", endpoint.password});
}

}  // namespace nexus
