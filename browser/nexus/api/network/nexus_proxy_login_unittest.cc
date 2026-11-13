// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/network/nexus_proxy_login.h"

#include <optional>
#include <string>
#include <utility>

#include "base/values.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/testing_pref_service.h"
#include "components/proxy_config/proxy_config_dictionary.h"
#include "components/proxy_config/proxy_config_pref_names.h"
#include "custom_browser/browser/nexus/api/network/nexus_proxy_settings.h"
#include "net/base/auth.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"
#include "url/scheme_host_port.h"

namespace nexus {
namespace {

class NexusProxyLoginTest : public testing::Test {
 protected:
  NexusProxyLoginTest() {
    prefs_.registry()->RegisterDictionaryPref(
        proxy_config::prefs::kProxy, ProxyConfigDictionary::CreateSystem());
    prefs_.registry()->RegisterDictionaryPref(kProxyAuthPref);
    prefs_.SetDict(proxy_config::prefs::kProxy,
                   ProxyConfigDictionary::CreateFixedServers(
                       "proxy.example.com:3128", ""));
    base::DictValue sign_in;
    sign_in.Set("server", "proxy.example.com:3128");
    sign_in.Set("username", "me");
    // An unsealed value reads back as it is.
    sign_in.Set("password", "s3cret-pass");
    prefs_.SetDict(kProxyAuthPref, std::move(sign_in));
  }

  static net::AuthChallengeInfo Challenge(const std::string& origin,
                                          bool is_proxy) {
    net::AuthChallengeInfo info;
    info.is_proxy = is_proxy;
    info.challenger = url::SchemeHostPort(GURL(origin));
    info.scheme = "basic";
    return info;
  }

  TestingPrefServiceSimple prefs_;
};

TEST_F(NexusProxyLoginTest, TheUsersProxyIsAnsweredWithTheStoredSignIn) {
  const std::optional<net::AuthCredentials> credentials =
      StoredProxyChallengeCredentials(
          prefs_, Challenge("http://proxy.example.com:3128", true),
          /*first_auth_attempt=*/true);
  ASSERT_TRUE(credentials.has_value());
  EXPECT_EQ(credentials->username(), u"me");
  EXPECT_EQ(credentials->password(), u"s3cret-pass");
}

// The password goes to the configured proxy's first challenge and nowhere
// else: not to a web server, another proxy, or a second attempt.
TEST_F(NexusProxyLoginTest, NothingElseIsAnswered) {
  EXPECT_FALSE(StoredProxyChallengeCredentials(
      prefs_, Challenge("http://proxy.example.com:3128", false), true));
  EXPECT_FALSE(StoredProxyChallengeCredentials(
      prefs_, Challenge("http://evil.example.com:3128", true), true));
  EXPECT_FALSE(StoredProxyChallengeCredentials(
      prefs_, Challenge("http://proxy.example.com:3128", true), false));
}

TEST_F(NexusProxyLoginTest, NoStoredSignInAnswersNothing) {
  prefs_.ClearPref(kProxyAuthPref);
  EXPECT_FALSE(StoredProxyChallengeCredentials(
      prefs_, Challenge("http://proxy.example.com:3128", true), true));
}

}  // namespace
}  // namespace nexus
