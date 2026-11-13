// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/llm/llm_egress_policy.h"

#include <string>
#include <utility>

#include "base/values.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

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
  const auto endpoint = ResolveEgressEndpoint(egress);
  return endpoint.has_value() ? std::string() : endpoint.error();
}

TEST(LlmEgressPolicyTest, AUrlResolvesToAServerAndItsCredentials) {
  const auto endpoint = ResolveEgressEndpoint(Egress(
      "http://user:p%40ss@proxy.example.com:3128", {"Proxy.Example.com"}));
  ASSERT_TRUE(endpoint.has_value());
  EXPECT_TRUE(endpoint->server.is_http());
  EXPECT_EQ(endpoint->server.GetHost(), "proxy.example.com");
  EXPECT_EQ(endpoint->server.GetPort(), 3128);
  EXPECT_EQ(endpoint->username, "user");
  EXPECT_EQ(endpoint->password, "p@ss");
}

// No stored secret can be sent to a proxy: a placeholder is refused, whole
// address or one part, whatever it names.
TEST(LlmEgressPolicyTest, APlaceholderInTheUrlIsRefused) {
  EXPECT_EQ(ReasonOf(Egress("{{secret:proxy_url}}", {"proxy.example.com"})),
            "template");
  EXPECT_EQ(ReasonOf(Egress("http://{{secret:api_key}}:x@proxy.example.com",
                            {"proxy.example.com"})),
            "template");
  EXPECT_EQ(ReasonOf(Egress("https://me:{{oauth:access_token}}@proxy.example.com",
                            {"proxy.example.com"})),
            "template");
}

TEST(LlmEgressPolicyTest, TheProxyHostMustBeOneOfTheListedHosts) {
  EXPECT_EQ(ReasonOf(Egress("http://user:pw@proxy.example.com:3128",
                            {"other.example.com"})),
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

TEST(LlmEgressPolicyTest, TheContextKeySeparatesServersAndCredentials) {
  const auto resolve = [](const std::string& url) {
    return *ResolveEgressEndpoint(Egress(url, {"proxy.example.com"}));
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
