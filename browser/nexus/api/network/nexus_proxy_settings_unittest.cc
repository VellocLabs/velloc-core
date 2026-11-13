// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/network/nexus_proxy_settings.h"

#include <string>
#include <utility>

#include "base/values.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/testing_pref_service.h"
#include "components/proxy_config/proxy_config_dictionary.h"
#include "components/proxy_config/proxy_config_pref_names.h"
#include "custom_browser/browser/nexus/api/network/nexus_proxy_endpoint.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

ProxySettingsRequest Fixed(const std::string& server) {
  ProxySettingsRequest request;
  request.mode = "fixed";
  request.server = server;
  return request;
}

std::string RefusalOf(const ProxySettingsRequest& request) {
  const auto value = BuildProxyPreference(request);
  return value.has_value()
             ? std::string()
             : value.error().field + ":" + value.error().reason;
}

class ProxyPreferenceTest : public testing::Test {
 protected:
  ProxyPreferenceTest() {
    prefs_.registry()->RegisterDictionaryPref(
        proxy_config::prefs::kProxy, ProxyConfigDictionary::CreateSystem());
  }

  void Store(const ProxySettingsRequest& request) {
    auto value = BuildProxyPreference(request);
    ASSERT_TRUE(value.has_value());
    ASSERT_TRUE(value->has_value());
    prefs_.SetDict(proxy_config::prefs::kProxy, std::move(**value));
  }

  TestingPrefServiceSimple prefs_;
};

TEST(ProxyEndpointTest, ParsesEachSchemeWithItsDefaultPort) {
  auto http = ParseProxyEndpoint(" http://proxy.example.com/ ",
                                 ProxyCredentials::kRefuse);
  ASSERT_TRUE(http.has_value());
  EXPECT_TRUE(http->server.is_http());
  EXPECT_EQ(http->server.GetHost(), "proxy.example.com");
  EXPECT_EQ(http->server.GetPort(), 80);

  auto socks = ParseProxyEndpoint("SOCKS5://10.0.0.1:1081",
                                  ProxyCredentials::kRefuse);
  ASSERT_TRUE(socks.has_value());
  EXPECT_EQ(socks->server.scheme(), net::ProxyServer::SCHEME_SOCKS5);
  EXPECT_EQ(socks->server.GetPort(), 1081);

  auto https = ParseProxyEndpoint("https://[2001:db8::1]:8443",
                                  ProxyCredentials::kRefuse);
  ASSERT_TRUE(https.has_value());
  EXPECT_TRUE(https->server.is_https());
  EXPECT_EQ(https->server.GetPort(), 8443);
}

TEST(ProxyEndpointTest, RefusesWhatIsNotOneProxyServer) {
  const auto problem = [](std::string_view text) {
    return ParseProxyEndpoint(text, ProxyCredentials::kAllowForHttp).error();
  };
  EXPECT_EQ(problem("proxy.example.com:8080"),
            ProxyEndpointProblem::kMalformed);
  EXPECT_EQ(problem("http://"), ProxyEndpointProblem::kMalformed);
  EXPECT_EQ(problem("http://proxy.example.com/path"),
            ProxyEndpointProblem::kMalformed);
  EXPECT_EQ(problem("http://a.example.com http://b.example.com"),
            ProxyEndpointProblem::kMalformed);
  EXPECT_EQ(problem("http://:secret@proxy.example.com"),
            ProxyEndpointProblem::kMalformed);
  EXPECT_EQ(problem("ftp://proxy.example.com"), ProxyEndpointProblem::kScheme);
  EXPECT_EQ(problem("socks://proxy.example.com"),
            ProxyEndpointProblem::kScheme);
}

TEST(ProxyEndpointTest, CredentialsAreReadOnlyWhereTheyCanBeAnswered) {
  auto allowed = ParseProxyEndpoint("http://us%40er:p%3Ass@proxy.example.com:3128",
                                    ProxyCredentials::kAllowForHttp);
  ASSERT_TRUE(allowed.has_value());
  EXPECT_EQ(allowed->username, "us@er");
  EXPECT_EQ(allowed->password, "p:ss");
  EXPECT_EQ(allowed->server.GetHost(), "proxy.example.com");
  EXPECT_EQ(allowed->server.GetPort(), 3128);

  EXPECT_EQ(ParseProxyEndpoint("http://user:pass@proxy.example.com",
                               ProxyCredentials::kRefuse)
                .error(),
            ProxyEndpointProblem::kCredentials);
  EXPECT_EQ(ParseProxyEndpoint("socks5://user:pass@proxy.example.com",
                               ProxyCredentials::kAllowForHttp)
                .error(),
            ProxyEndpointProblem::kCredentials);
}

TEST(ProxySettingsTest, SystemStoresNothing) {
  ProxySettingsRequest request;
  request.mode = "system";
  const auto value = BuildProxyPreference(request);
  ASSERT_TRUE(value.has_value());
  EXPECT_FALSE(value->has_value());
}

TEST(ProxySettingsTest, RefusalsNameTheFieldAndTheReason) {
  EXPECT_EQ(RefusalOf(Fixed("")), "server:malformed");
  EXPECT_EQ(RefusalOf(Fixed("ftp://proxy.example.com")), "server:scheme");
  EXPECT_EQ(RefusalOf(Fixed("http://user:pass@proxy.example.com")),
            "server:credentials");

  ProxySettingsRequest long_bypass = Fixed("http://proxy.example.com");
  long_bypass.bypass_list = std::string(kMaxProxyBypassListChars + 1, 'a');
  EXPECT_EQ(RefusalOf(long_bypass), "bypassList:tooLong");

  ProxySettingsRequest split_bypass = Fixed("http://proxy.example.com");
  split_bypass.bypass_list = "a.example.com\nb.example.com";
  EXPECT_EQ(RefusalOf(split_bypass), "bypassList:malformed");

  ProxySettingsRequest pac;
  pac.mode = "pac";
  pac.pac_url = "ftp://example.com/proxy.pac";
  EXPECT_EQ(RefusalOf(pac), "pacUrl:scheme");
  pac.pac_url = "not a url";
  EXPECT_EQ(RefusalOf(pac), "pacUrl:malformed");

  ProxySettingsRequest unknown;
  unknown.mode = "autoDetect";
  EXPECT_EQ(RefusalOf(unknown), "mode:malformed");
}

TEST_F(ProxyPreferenceTest, AnUnsetPreferenceReadsAsTheSystemDefault) {
  const base::DictValue record = DescribeProxyPreference(prefs_);
  EXPECT_EQ(*record.FindString("mode"), "system");
  EXPECT_EQ(*record.FindString("source"), "default");
  EXPECT_EQ(record.FindBool("editable"), true);
  EXPECT_FALSE(record.contains("server"));
  EXPECT_TRUE(IsProxyPreferenceEditable(prefs_));
}

TEST_F(ProxyPreferenceTest, AFixedServerReadsBackWithItsSchemeAndBypassList) {
  ProxySettingsRequest request = Fixed("http://proxy.example.com:3128");
  request.bypass_list = " *.internal.example.com;<local> ";
  Store(request);
  base::DictValue record = DescribeProxyPreference(prefs_);
  EXPECT_EQ(*record.FindString("mode"), "fixed");
  EXPECT_EQ(*record.FindString("server"), "http://proxy.example.com:3128");
  EXPECT_EQ(*record.FindString("bypassList"), "*.internal.example.com;<local>");
  EXPECT_EQ(*record.FindString("source"), "user");

  Store(Fixed("socks5://127.0.0.1:1080"));
  record = DescribeProxyPreference(prefs_);
  EXPECT_EQ(*record.FindString("server"), "socks5://127.0.0.1:1080");
}

TEST_F(ProxyPreferenceTest, DirectAndPacReadBack) {
  ProxySettingsRequest direct;
  direct.mode = "direct";
  Store(direct);
  EXPECT_EQ(*DescribeProxyPreference(prefs_).FindString("mode"), "direct");

  ProxySettingsRequest pac;
  pac.mode = "pac";
  pac.pac_url = "https://example.com/proxy.pac";
  Store(pac);
  const base::DictValue record = DescribeProxyPreference(prefs_);
  EXPECT_EQ(*record.FindString("mode"), "pac");
  EXPECT_EQ(*record.FindString("pacUrl"), "https://example.com/proxy.pac");
}

TEST_F(ProxyPreferenceTest, AValueHeldAboveTheUserStoreIsNotEditable) {
  Store(Fixed("http://mine.example.com:8080"));
  prefs_.SetManagedPref(
      proxy_config::prefs::kProxy,
      base::Value(ProxyConfigDictionary::CreateFixedServers(
          "policy.example.com:8080", std::string())));
  base::DictValue record = DescribeProxyPreference(prefs_);
  EXPECT_EQ(*record.FindString("server"), "http://policy.example.com:8080");
  EXPECT_EQ(*record.FindString("source"), "policy");
  EXPECT_EQ(record.FindBool("editable"), false);
  EXPECT_FALSE(IsProxyPreferenceEditable(prefs_));

  prefs_.RemoveManagedPref(proxy_config::prefs::kProxy);
  prefs_.SetExtensionPref(proxy_config::prefs::kProxy,
                          base::Value(ProxyConfigDictionary::CreateDirect()));
  record = DescribeProxyPreference(prefs_);
  EXPECT_EQ(*record.FindString("source"), "extension");
  EXPECT_EQ(record.FindBool("editable"), false);
}

}  // namespace
}  // namespace nexus
