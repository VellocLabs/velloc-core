// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_EGRESS_POLICY_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_EGRESS_POLICY_H_

#include <optional>
#include <string>

#include "base/types/expected.h"
#include "base/values.h"
#include "custom_browser/browser/nexus/api/llm/llm_raw_stream_policy.h"
#include "custom_browser/browser/nexus/api/network/nexus_proxy_endpoint.h"

namespace nexus {

// The `egress` input of `model.rawStreamOpen`: the proxy one provider request
// leaves through (P-4).
//
//   { "url": "http://{{secret:proxy_user}}:{{secret:proxy_pass}}@host:3128",
//     "hosts": ["host"] }

// Why an egress was refused. Exactly one of the two is set.
struct EgressRefusal {
  EgressRefusal();
  EgressRefusal(EgressRefusal&&);
  EgressRefusal& operator=(EgressRefusal&&);
  ~EgressRefusal();

  // The wire reason of an EGRESS_INVALID: `malformed`, `scheme`,
  // `credentials`, or `host` (the proxy's host is not one of `hosts`).
  std::string reason;
  // A `{{secret:NAME}}` in the URL that could not be filled.
  std::optional<SecretTemplateError> secret;
};

// Contract: `url` after `{{secret:NAME}}` substitution must be one proxy
// server (http, https or socks5; credentials on http and https only).
// Security: the proxy's host must be one of `hosts`, compared without case —
// the list is what the person consented to, and the expanded URL is the only
// place the real host appears, so the check has to be made here. The expanded
// text never leaves this function except inside the returned endpoint. Pure.
base::expected<ProxyEndpoint, EgressRefusal> ResolveEgressEndpoint(
    const base::DictValue& egress,
    const SecretLookup& lookup);

// A key under which two endpoints share one network context: the same proxy
// server answered with the same credentials.
std::string EgressContextKey(const ProxyEndpoint& endpoint);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_EGRESS_POLICY_H_
