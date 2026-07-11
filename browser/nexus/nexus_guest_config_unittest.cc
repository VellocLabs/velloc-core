// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/nexus_guest_config.h"

#include <memory>
#include <string>

#include "chrome/browser/prefs/browser_prefs.h"
#include "chrome/test/base/testing_profile.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "components/sync_preferences/testing_pref_service_syncable.h"
#include "content/public/test/browser_task_environment.h"
#include "custom_browser/browser/nexus/api/llm/llm_pref_names.h"
#include "custom_browser/browser/nexus/api/llm/llm_pref_registration.h"
#include "custom_browser/browser/nexus/nexus_test_support.h"
#include "custom_browser/browser/nexus/nexus_webui_config.h"
#include "net/base/url_util.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace nexus {
namespace {

std::unique_ptr<TestingProfile> CreateTestingProfileWithNexusPrefs() {
  auto prefs =
      std::make_unique<sync_preferences::TestingPrefServiceSyncable>();
  // TestingProfile::Init builds standard profile machinery (notably
  // ExtensionPrefs) that reads many standard profile prefs, so a supplied pref
  // store must carry the full profile pref set — not just the nexus prefs under
  // test. (With a caller-supplied PrefService, TestingProfile does not run the
  // keyed-service pref registration itself.)
  RegisterUserProfilePrefs(prefs->registry());
  RegisterNexusProfilePrefs(prefs->registry());

  TestingProfile::Builder builder;
  builder.SetPrefService(std::move(prefs));
  // Building a regular TestingProfile would otherwise eagerly construct
  // GlicKeyedService, which crashes without a real ProfileManager. See
  // nexus_test_support.h.
  test::AddGlicDisablingFactory(builder);
  return builder.Build();
}

// TestingProfile creation needs a content task environment (UI/IO threads).
class NexusGuestConfigTest : public testing::Test {
 private:
  content::BrowserTaskEnvironment task_environment_;
};

TEST_F(NexusGuestConfigTest, GetResolvedNexusGuestURLFallsBackWithoutProfile) {
  EXPECT_EQ(GURL(kDefaultNexusGuestURL), GetResolvedNexusGuestURL(nullptr));
}

TEST_F(NexusGuestConfigTest, GetResolvedNexusGuestURLAcceptsConfiguredHttpsUrl) {
  std::unique_ptr<TestingProfile> profile = CreateTestingProfileWithNexusPrefs();
  profile->GetPrefs()->SetString(prefs::kNexusWebappUrl,
                                 "https://example.com/agent");

  EXPECT_EQ(GURL("https://example.com/agent"),
            GetResolvedNexusGuestURL(profile.get()));
}

TEST_F(NexusGuestConfigTest, GetResolvedNexusGuestURLAcceptsLocalhostDevUrls) {
  std::unique_ptr<TestingProfile> profile = CreateTestingProfileWithNexusPrefs();

  profile->GetPrefs()->SetString(prefs::kNexusWebappUrl,
                                 "http://localhost:3000/app");
  EXPECT_EQ(GURL("http://localhost:3000/app"),
            GetResolvedNexusGuestURL(profile.get()));

  profile->GetPrefs()->SetString(prefs::kNexusWebappUrl,
                                 "http://127.0.0.1:4173/app");
  EXPECT_EQ(GURL("http://127.0.0.1:4173/app"),
            GetResolvedNexusGuestURL(profile.get()));
}

TEST_F(NexusGuestConfigTest, GetResolvedNexusGuestURLRejectsInvalidCustomUrls) {
  std::unique_ptr<TestingProfile> profile = CreateTestingProfileWithNexusPrefs();

  profile->GetPrefs()->SetString(prefs::kNexusWebappUrl,
                                 "http://example.com/not-allowed");
  EXPECT_EQ(GURL(kDefaultNexusGuestURL),
            GetResolvedNexusGuestURL(profile.get()));

  profile->GetPrefs()->SetString(prefs::kNexusWebappUrl, "ftp://localhost/app");
  EXPECT_EQ(GURL(kDefaultNexusGuestURL),
            GetResolvedNexusGuestURL(profile.get()));
}

// Per-skill deferral is configured SOLELY by the session template's
// `deferredTools` (nexus_web: resolveDeferrableSkills, from
// src/template/<type>/template.md), NOT by settings. The old `defer.<skill>`
// settings + their guest-URL composition were removed. So even when legacy
// `defer.<skill>` keys linger in agent_config, GetResolvedNexusGuestURL must
// NOT compose any `defer<Skill>` query param. (Regression: this composition
// used to emit deferBrowser/deferAgentTeams=0|1 from these prefs.)
TEST_F(NexusGuestConfigTest,
     GetResolvedNexusGuestURLDoesNotComposeDeferralFromSettings) {
  std::unique_ptr<TestingProfile> profile = CreateTestingProfileWithNexusPrefs();
  profile->GetPrefs()->SetString(prefs::kNexusWebappUrl,
                                 "http://localhost:5173/");

  {
    ScopedDictPrefUpdate update(profile->GetPrefs(), prefs::kNexusAgentConfig);
    update->Set("defer.browser", true);
    update->Set("defer.agentTeams", false);
  }

  const GURL url = GetResolvedNexusGuestURL(profile.get());

  std::string value;
  EXPECT_FALSE(net::GetValueForKeyInQuery(url, "deferBrowser", &value));
  EXPECT_FALSE(net::GetValueForKeyInQuery(url, "deferA2ui", &value));
  EXPECT_FALSE(net::GetValueForKeyInQuery(url, "deferMcp", &value));
  EXPECT_FALSE(net::GetValueForKeyInQuery(url, "deferAgentTeams", &value));
}

// `log` / `trace` were the only `agent_config`-driven query params left after
// the deferral cleanup. They've now been removed too: the nexus_web page-thread
// reads those flags directly from `~/.nexus/settings.json` (under
// `agent.log` / `agent.trace`) via `loadAgentTracerFlags()`. Even when the
// legacy pref carries them, the resolved URL must NOT inject them — otherwise
// the C++ pref would silently shadow whatever the user wrote in the new
// surface. (Regression: this composition used to emit `?log=1&trace=1` from
// these prefs.)
TEST_F(NexusGuestConfigTest,
     GetResolvedNexusGuestURLDoesNotComposeTracerFlagsFromAgentConfigPref) {
  std::unique_ptr<TestingProfile> profile = CreateTestingProfileWithNexusPrefs();
  profile->GetPrefs()->SetString(prefs::kNexusWebappUrl,
                                 "http://localhost:5173/");

  {
    ScopedDictPrefUpdate update(profile->GetPrefs(), prefs::kNexusAgentConfig);
    update->Set("log", true);
    update->Set("trace", true);
  }

  const GURL url = GetResolvedNexusGuestURL(profile.get());

  std::string value;
  EXPECT_FALSE(net::GetValueForKeyInQuery(url, "log", &value));
  EXPECT_FALSE(net::GetValueForKeyInQuery(url, "trace", &value));
  // The base URL itself is preserved verbatim — nothing was appended.
  EXPECT_EQ(GURL("http://localhost:5173/"), url);
}

}  // namespace
}  // namespace nexus
