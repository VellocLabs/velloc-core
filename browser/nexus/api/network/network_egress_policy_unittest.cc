// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/network/network_egress_policy.h"

#include <string>

#include "net/base/ip_address.h"
#include "net/base/net_errors.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace nexus {
namespace {

net::IPAddress Ip(const std::string& literal) {
  net::IPAddress address;
  CHECK(address.AssignFromIPLiteral(literal)) << literal;
  return address;
}

TEST(NetworkEgressPolicyTest, PublicAddressesAreAllowed) {
  EXPECT_TRUE(IsPublicEgressAddress(Ip("8.8.8.8")));
  EXPECT_TRUE(IsPublicEgressAddress(Ip("93.184.216.34")));
  EXPECT_TRUE(IsPublicEgressAddress(Ip("2606:4700:4700::1111")));
  EXPECT_TRUE(IsPublicEgressAddress(Ip("::ffff:8.8.8.8")));
  EXPECT_FALSE(EgressRefusalFor(Ip("1.1.1.1")).has_value());
}

TEST(NetworkEgressPolicyTest, LoopbackAndUnspecifiedAreRefused) {
  EXPECT_EQ("loopback address", *EgressRefusalFor(Ip("127.0.0.1")));
  EXPECT_EQ("loopback address", *EgressRefusalFor(Ip("127.255.255.254")));
  EXPECT_EQ("loopback address", *EgressRefusalFor(Ip("::1")));
  EXPECT_EQ("unspecified (0.0.0.0/8) address", *EgressRefusalFor(Ip("0.0.0.0")));
  EXPECT_EQ("unspecified (::) address", *EgressRefusalFor(Ip("::")));
}

TEST(NetworkEgressPolicyTest, PrivateRangesAreRefused) {
  EXPECT_EQ("private network address", *EgressRefusalFor(Ip("10.0.0.1")));
  EXPECT_EQ("private network address", *EgressRefusalFor(Ip("172.16.0.1")));
  EXPECT_EQ("private network address", *EgressRefusalFor(Ip("172.31.255.255")));
  EXPECT_EQ("private network address", *EgressRefusalFor(Ip("192.168.1.1")));
  EXPECT_EQ("unique-local (fc00::/7) address", *EgressRefusalFor(Ip("fd00::1")));
  EXPECT_EQ("carrier-grade NAT (100.64/10) address",
            *EgressRefusalFor(Ip("100.64.0.1")));
  EXPECT_EQ("carrier-grade NAT (100.64/10) address",
            *EgressRefusalFor(Ip("100.127.255.255")));
  // 100.128.0.0 is just past the /10: public.
  EXPECT_TRUE(IsPublicEgressAddress(Ip("100.128.0.1")));
}

TEST(NetworkEgressPolicyTest, LinkLocalAndMetadataAreRefused) {
  EXPECT_EQ("link-local (169.254/16) address",
            *EgressRefusalFor(Ip("169.254.169.254")));
  EXPECT_EQ("link-local (fe80::/10) address", *EgressRefusalFor(Ip("fe80::1")));
  EXPECT_EQ("multicast or reserved address", *EgressRefusalFor(Ip("224.0.0.1")));
  EXPECT_EQ("multicast or reserved address",
            *EgressRefusalFor(Ip("255.255.255.255")));
  EXPECT_EQ("multicast address", *EgressRefusalFor(Ip("ff02::1")));
}

TEST(NetworkEgressPolicyTest, MappedIPv6JudgesTheEmbeddedIPv4) {
  EXPECT_EQ("loopback address", *EgressRefusalFor(Ip("::ffff:127.0.0.1")));
  EXPECT_EQ("link-local (169.254/16) address",
            *EgressRefusalFor(Ip("::ffff:169.254.169.254")));
  EXPECT_EQ("private network address", *EgressRefusalFor(Ip("::ffff:10.1.2.3")));
}

TEST(NetworkEgressPolicyTest, AnInvalidAddressIsNotARefusal) {
  // No endpoint was dialed (a cached response): nothing to refuse.
  EXPECT_FALSE(EgressRefusalFor(net::IPAddress()).has_value());
  EXPECT_TRUE(IsPublicEgressAddress(net::IPAddress()));
}

TEST(NetworkEgressPolicyTest, LiteralHostsAreCheckedBeforeDialing) {
  EXPECT_EQ("host 127.0.0.1 is a loopback address",
            *EgressRefusalForLiteralHost(GURL("http://127.0.0.1:1/")));
  EXPECT_EQ("host 169.254.169.254 is a link-local (169.254/16) address",
            *EgressRefusalForLiteralHost(
                GURL("http://169.254.169.254/latest/meta-data/")));
  EXPECT_EQ("host [::1] is a loopback address",
            *EgressRefusalForLiteralHost(GURL("http://[::1]:8080/")));
  EXPECT_FALSE(EgressRefusalForLiteralHost(GURL("http://8.8.8.8/")).has_value());
  // A hostname is judged after resolution, not here.
  EXPECT_FALSE(EgressRefusalForLiteralHost(GURL("https://localhost/")).has_value());
  EXPECT_FALSE(EgressRefusalForLiteralHost(GURL("https://example.com/")).has_value());
  EXPECT_FALSE(EgressRefusalForLiteralHost(GURL("not a url")).has_value());
}

// The network service refuses a loopback / private peer before the request
// is written (kURLLoadOptionBlockLocalRequest); that failure is the policy's
// refusal, any other net error is not.
TEST(NetworkEgressPolicyTest, LocalNetworkBlockNetErrorIsARefusal) {
  EXPECT_EQ(
      "lan.example resolved to a loopback or private network address; the "
      "connection was refused before the request was sent",
      *EgressRefusalForNetError(net::ERR_BLOCKED_BY_LOCAL_NETWORK_ACCESS_CHECKS,
                                GURL("http://lan.example:8080/cmd")));
  EXPECT_FALSE(EgressRefusalForNetError(net::ERR_CONNECTION_REFUSED,
                                        GURL("http://lan.example/"))
                   .has_value());
  EXPECT_FALSE(
      EgressRefusalForNetError(net::OK, GURL("http://lan.example/")).has_value());
}

}  // namespace
}  // namespace nexus
