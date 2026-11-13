// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_EGRESS_POLICY_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_EGRESS_POLICY_H_

#include <string>

#include "base/types/expected.h"
#include "base/values.h"
#include "custom_browser/browser/nexus/api/network/nexus_proxy_endpoint.h"

namespace nexus {

// The `egress` input of `model.rawStreamOpen`: the proxy one provider request
// leaves through (P-4).
//
//   { "url": "http://user:password@host:3128", "hosts": ["host"] }

// Contract: `url` must be one proxy server (http, https or socks5;
// credentials on http and https only), and its host one of `hosts`, compared
// without case. The error is the wire reason of an EGRESS_INVALID:
// `malformed`, `template`, `scheme`, `credentials` or `host`. Pure.
// Security: the address is used as written. A `{{…}}` placeholder is
// refused (`template`) and no stored secret is ever filled in: the proxy is
// a host the caller chose, so a secret placed in its address would be
// handed to that host.
base::expected<ProxyEndpoint, std::string> ResolveEgressEndpoint(
    const base::DictValue& egress);

// A key under which two endpoints share one network context: the same proxy
// server answered with the same credentials.
std::string EgressContextKey(const ProxyEndpoint& endpoint);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_EGRESS_POLICY_H_
