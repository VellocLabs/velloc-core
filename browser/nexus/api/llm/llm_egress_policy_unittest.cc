// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/llm/llm_egress_policy.h"

#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/values.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

SecretLookupOutcome Lookup(const std::string& name) {
  SecretLookupOutcome outcome;
  if (name == "proxy_url") {
    outcome.value = "http://user:p%40ss@proxy.example.com:3128";
  } else if (name == "proxy_pass") {
    outcome.value = "s3cret";
  }
  return outcome;
}

base::DictValue Egress(const std::string& url,
                       std::initializer_list<const char*> hosts) {
  base::DictValue egress;
  egress.Set("url", url);
  base::ListValue list;
  for (const char* host : hosts) {
    list.Append(host);
  }
  egress.Set("hosts", std::move(list));
  return egress;
}

std::string ReasonOf(const base::DictValue& egress) {
  const auto endpoint =
      ResolveEgressEndpoint(egress, base::BindRepeating(&Lookup));
  return endpoint.has_value() ? std::string() : endpoint.error().reason;
}

TEST(LlmEgressPolicyTest, ASecretUrlResolvesToAServerAndItsCredentials) {
  const auto endpoint =
      ResolveEgressEndpoint(Egress("{{secret:proxy_url}}", {"Proxy.Example.com"}),
                            base::BindRepeating(&Lookup));
  ASSERT_TRUE(endpoint.has_value());
  EXPECT_TRUE(endpoint->server.is_http());
  EXPECT_EQ(endpoint->server.GetHost(), "proxy.example.com");
  EXPECT_EQ(endpoint->server.GetPort(), 3128);
  EXPECT_EQ(endpoint->username, "user");
  EXPECT_EQ(endpoint->password, "p@ss");
}

TEST(LlmEgressPolicyTest, ASecretMayFillOnePartOfTheUrl) {
  const auto endpoint = ResolveEgressEndpoint(
      Egress("https://me:{{secret:proxy_pass}}@gate.example.com", {"gate.example.com"}),
      base::BindRepeating(&Lookup));
  ASSERT_TRUE(endpoint.has_value());
  EXPECT_TRUE(endpoint->server.is_https());
  EXPECT_EQ(endpoint->server.GetPort(), 443);
  EXPECT_EQ(endpoint->password, "s3cret");
}

TEST(LlmEgressPolicyTest, TheProxyHostMustBeOneOfTheListedHosts) {
  EXPECT_EQ(ReasonOf(Egress("{{secret:proxy_url}}", {"other.example.com"})),
            "host");
  EXPECT_EQ(ReasonOf(Egress("http://evil.example.com:8080",
                            {"proxy.example.com", "example.com"})),
            "host");
  EXPECT_EQ(ReasonOf(Egress("socks5://[2001:db8::1]:1080", {"2001:db8::1"})),
            "");
}

TEST(LlmEgressPolicyTest, RefusalsCarryTheirReason) {
  EXPECT_EQ(ReasonOf(base::DictValue()), "malformed");
  EXPECT_EQ(ReasonOf(Egress("http://proxy.example.com", {})), "malformed");
  EXPECT_EQ(ReasonOf(Egress("proxy.example.com:8080", {"proxy.example.com"})),
            "malformed");
  EXPECT_EQ(ReasonOf(Egress("socks4://proxy.example.com", {"proxy.example.com"})),
            "scheme");
  EXPECT_EQ(ReasonOf(Egress("socks5://u:p@proxy.example.com",
                            {"proxy.example.com"})),
            "credentials");
}

TEST(LlmEgressPolicyTest, AMissingSecretIsReportedAsTheSecretNotTheUrl) {
  const auto endpoint = ResolveEgressEndpoint(
      Egress("{{secret:absent}}", {"proxy.example.com"}),
      base::BindRepeating(&Lookup));
  ASSERT_FALSE(endpoint.has_value());
  EXPECT_TRUE(endpoint.error().reason.empty());
  ASSERT_TRUE(endpoint.error().secret.has_value());
  EXPECT_EQ(endpoint.error().secret->secret, "absent");
}

TEST(LlmEgressPolicyTest, TheContextKeySeparatesServersAndCredentials) {
  const auto resolve = [](const std::string& url) {
    return *ResolveEgressEndpoint(Egress(url, {"proxy.example.com"}),
                                  base::BindRepeating(&Lookup));
  };
  const std::string plain = EgressContextKey(resolve("http://proxy.example.com:80"));
  EXPECT_EQ(plain, EgressContextKey(resolve("HTTP://proxy.example.com/")));
  EXPECT_NE(plain, EgressContextKey(resolve("http://proxy.example.com:81")));
  EXPECT_NE(plain, EgressContextKey(resolve("https://proxy.example.com:80")));
  EXPECT_NE(plain, EgressContextKey(resolve("http://a:b@proxy.example.com")));
  EXPECT_NE(EgressContextKey(resolve("http://a:b@proxy.example.com")),
            EgressContextKey(resolve("http://a:c@proxy.example.com")));
}

}  // namespace
}  // namespace nexus
