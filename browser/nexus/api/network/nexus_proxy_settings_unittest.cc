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
#include "url/gurl.h"
#include "url/scheme_host_port.h"

namespace nexus {
namespace {

url::SchemeHostPort Proxy(const std::string& url) {
  return url::SchemeHostPort(GURL(url));
}

ProxySettingsRequest Fixed(const std::string& server) {
  ProxySettingsRequest request;
  request.mode = "fixed";
  request.server = server;
  return request;
}

base::DictValue SignIn(const std::string& server,
                       const std::string& username,
                       const std::string& password) {
  base::DictValue stored;
  stored.Set("server", server);
  stored.Set("username", username);
  stored.Set("password", password);
  return stored;
}

ProxySettingsRequest SignedIn(const std::string& server,
                              const std::string& username,
                              const std::string& password) {
  ProxySettingsRequest request = Fixed(server);
  request.username = username;
  if (!password.empty()) {
    request.password = password;
  }
  return request;
}

std::string SignInRefusalOf(const ProxySettingsRequest& request,
                            const base::DictValue& stored = {}) {
  const auto change = ResolveProxyAuthChange(request, stored);
  return change.has_value()
             ? std::string()
             : change.error().field + ":" + change.error().reason;
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
    prefs_.registry()->RegisterDictionaryPref(kProxyAuthPref);
  }

  void StoreSignIn(const std::string& server,
                   const std::string& username,
                   const std::string& password) {
    prefs_.SetDict(kProxyAuthPref, SignIn(server, username, password));
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
  pac.pac_url = "file:///C:/corp/proxy.pac";
  EXPECT_EQ(RefusalOf(pac), "pacUrl:scheme");
  pac.pac_url = "not a url";
  EXPECT_EQ(RefusalOf(pac), "pacUrl:malformed");

  ProxySettingsRequest unknown;
  unknown.mode = "auto";
  EXPECT_EQ(RefusalOf(unknown), "mode:malformed");
}

TEST(ProxySignInTest, AUserNameAndPasswordAreStoredForTheProxy) {
  const auto change = ResolveProxyAuthChange(
      SignedIn("http://Proxy.Example.com:3128", " me ", "s3cret"), {});
  ASSERT_TRUE(change.has_value());
  EXPECT_EQ(change->kind, ProxyAuthChange::Kind::kStore);
  EXPECT_EQ(change->server, "http://proxy.example.com:3128");
  EXPECT_EQ(change->username, "me");
  EXPECT_EQ(change->password, "s3cret");
}

TEST(ProxySignInTest, NoUserNameClearsTheStoredSignIn) {
  const auto fixed = ResolveProxyAuthChange(
      Fixed("http://proxy.example.com"),
      SignIn("http://proxy.example.com:80", "me", "sealed"));
  ASSERT_TRUE(fixed.has_value());
  EXPECT_EQ(fixed->kind, ProxyAuthChange::Kind::kClear);
  ProxySettingsRequest direct;
  direct.mode = "direct";
  const auto other = ResolveProxyAuthChange(direct, {});
  ASSERT_TRUE(other.has_value());
  EXPECT_EQ(other->kind, ProxyAuthChange::Kind::kClear);
}

// A password is kept only for the proxy and user name it was stored for.
TEST(ProxySignInTest, AnOmittedPasswordKeepsOnlyItsOwnProxysPassword) {
  const base::DictValue stored = SignIn("http://proxy.example.com:3128", "me", "sealed");
  const auto same = ResolveProxyAuthChange(
      SignedIn("http://proxy.example.com:3128", "me", ""), stored);
  ASSERT_TRUE(same.has_value());
  EXPECT_EQ(same->kind, ProxyAuthChange::Kind::kKeep);

  EXPECT_EQ(SignInRefusalOf(SignedIn("http://other.example.com:3128", "me", ""),
                            stored),
            "password:required");
  EXPECT_EQ(SignInRefusalOf(SignedIn("http://proxy.example.com:3129", "me", ""),
                            stored),
            "password:required");
  EXPECT_EQ(SignInRefusalOf(SignedIn("http://proxy.example.com:3128", "you", ""),
                            stored),
            "password:required");
  EXPECT_EQ(SignInRefusalOf(SignedIn("http://proxy.example.com:3128", "me", "")),
            "password:required");
  // The same address over plain http is another proxy.
  EXPECT_EQ(SignInRefusalOf(SignedIn("http://proxy.example.com:3128", "me", ""),
                            SignIn("https://proxy.example.com:3128", "me",
                                   "sealed")),
            "password:required");
}

TEST(ProxySignInTest, RefusalsNameTheFieldAndTheReason) {
  EXPECT_EQ(SignInRefusalOf(SignedIn("socks5://proxy.example.com", "me", "pw")),
            "username:scheme");
  EXPECT_EQ(SignInRefusalOf(SignedIn("http://proxy.example.com", "a:b", "pw")),
            "username:malformed");
  EXPECT_EQ(SignInRefusalOf(SignedIn(
                "http://proxy.example.com",
                std::string(kMaxProxyAuthChars + 1, 'a'), "pw")),
            "username:tooLong");
  EXPECT_EQ(SignInRefusalOf(SignedIn(
                "http://proxy.example.com", "me",
                std::string(kMaxProxyAuthChars + 1, 'a'))),
            "password:tooLong");
  EXPECT_EQ(SignInRefusalOf(SignedIn("http://proxy.example.com", "me", "a\nb")),
            "password:malformed");
  EXPECT_EQ(SignInRefusalOf(SignedIn("http://proxy.example.com", "", "pw")),
            "username:required");
  ProxySettingsRequest pac;
  pac.mode = "pac";
  pac.pac_url = "http://example.com/proxy.pac";
  pac.username = "me";
  pac.password = "pw";
  EXPECT_EQ(SignInRefusalOf(pac), "username:malformed");
}

TEST_F(ProxyPreferenceTest, AStoredSignInReadsBackWithoutItsPassword) {
  Store(Fixed("http://proxy.example.com:3128"));
  StoreSignIn("http://proxy.example.com:3128", "me", "sealed-text");
  const base::DictValue record = DescribeProxyPreference(prefs_);
  ASSERT_TRUE(record.FindString("username"));
  EXPECT_EQ(*record.FindString("username"), "me");
  EXPECT_EQ(record.FindBool("hasPassword"), true);
  EXPECT_FALSE(record.contains("password"));
  EXPECT_EQ(record.DebugString().find("sealed-text"), std::string::npos);

  // A sign-in written for another proxy is not this one's.
  Store(Fixed("http://other.example.com:3128"));
  EXPECT_FALSE(DescribeProxyPreference(prefs_).contains("username"));
}

TEST_F(ProxyPreferenceTest, TheSignInAnswersOnlyTheUsersOwnFixedProxy) {
  Store(Fixed("http://proxy.example.com:3128"));
  StoreSignIn("http://proxy.example.com:3128", "me", "sealed-text");
  const auto answer = StoredProxyAuthFor(
      prefs_, Proxy("http://proxy.example.com:3128"));
  ASSERT_TRUE(answer.has_value());
  EXPECT_EQ(answer->username, "me");
  EXPECT_EQ(answer->sealed_password, "sealed-text");

  EXPECT_FALSE(StoredProxyAuthFor(
      prefs_, Proxy("http://evil.example.com:3128")));
  EXPECT_FALSE(StoredProxyAuthFor(
      prefs_, Proxy("http://proxy.example.com:8080")));
  // The same address reached over another scheme is not the stored proxy.
  EXPECT_FALSE(StoredProxyAuthFor(
      prefs_, Proxy("https://proxy.example.com:3128")));

  // The setting changed to another proxy: the old sign-in is not offered.
  Store(Fixed("http://other.example.com:3128"));
  EXPECT_FALSE(StoredProxyAuthFor(
      prefs_, Proxy("http://other.example.com:3128")));
  EXPECT_FALSE(StoredProxyAuthFor(
      prefs_, Proxy("http://proxy.example.com:3128")));
}

TEST_F(ProxyPreferenceTest, AProxyHeldAboveTheUserStoreGetsNoSignIn) {
  Store(Fixed("http://proxy.example.com:3128"));
  StoreSignIn("http://proxy.example.com:3128", "me", "sealed-text");
  prefs_.SetManagedPref(
      proxy_config::prefs::kProxy,
      base::Value(ProxyConfigDictionary::CreateFixedServers(
          "proxy.example.com:3128", "")));
  EXPECT_FALSE(StoredProxyAuthFor(
      prefs_, Proxy("http://proxy.example.com:3128")));
  // Nor is it reported as in force.
  const base::DictValue record = DescribeProxyPreference(prefs_);
  EXPECT_FALSE(record.contains("username"));
  EXPECT_FALSE(record.contains("hasPassword"));
}

TEST_F(ProxyPreferenceTest, AnHttpsProxysSignInIsKeptForHttpsOnly) {
  Store(Fixed("https://proxy.example.com:3128"));
  StoreSignIn("https://proxy.example.com:3128", "me", "sealed-text");
  EXPECT_TRUE(StoredProxyAuthFor(
      prefs_, Proxy("https://proxy.example.com:3128")));
  EXPECT_EQ(DescribeProxyPreference(prefs_).FindBool("hasPassword"), true);

  Store(Fixed("http://proxy.example.com:3128"));
  EXPECT_FALSE(StoredProxyAuthFor(
      prefs_, Proxy("http://proxy.example.com:3128")));
  EXPECT_FALSE(DescribeProxyPreference(prefs_).contains("hasPassword"));
}

TEST_F(ProxyPreferenceTest, AutoDetectIsStoredAndReadBack) {
  ProxySettingsRequest request;
  request.mode = "autoDetect";
  Store(request);
  const base::DictValue record = DescribeProxyPreference(prefs_);
  EXPECT_EQ(*record.FindString("mode"), "autoDetect");
  EXPECT_EQ(*record.FindString("source"), "user");
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
