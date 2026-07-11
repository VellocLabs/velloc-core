// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/nexus_guest_config.h"

#include <memory>

#include "chrome/test/base/testing_profile.h"
#include "components/prefs/pref_service.h"
#include "components/sync_preferences/testing_pref_service_syncable.h"
#include "custom_browser/browser/nexus/llm/llm_pref_registration.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace nexus {
namespace {

std::unique_ptr<TestingProfile> CreateTestingProfileWithNexusPrefs() {
  auto prefs =
      std::make_unique<sync_preferences::TestingPrefServiceSyncable>();
  RegisterNexusProfilePrefs(prefs->registry());

  TestingProfile::Builder builder;
  builder.SetPrefService(std::move(prefs));
  return builder.Build();
}

TEST(NexusGuestConfigTest, IsNexusGlicModeMatchesOpenclawQuery) {
  EXPECT_TRUE(IsNexusGlicMode(GURL("chrome://glic/?openclaw=1")));
  EXPECT_TRUE(IsNexusGlicMode(GURL("chrome://glic/?foo=bar&openclaw=1")));
  EXPECT_FALSE(IsNexusGlicMode(GURL("chrome://glic/")));
  EXPECT_FALSE(IsNexusGlicMode(GURL("chrome://glic/?openclaw=0")));
  EXPECT_FALSE(IsNexusGlicMode(GURL("chrome://settings/?openclaw=1")));
}

TEST(NexusGuestConfigTest, GetResolvedNexusGuestURLFallsBackWithoutProfile) {
  EXPECT_EQ(GURL(kDefaultNexusGuestURL), GetResolvedNexusGuestURL(nullptr));
}

TEST(NexusGuestConfigTest, GetResolvedNexusGuestURLAcceptsConfiguredHttpsUrl) {
  std::unique_ptr<TestingProfile> profile = CreateTestingProfileWithNexusPrefs();
  profile->GetPrefs()->SetString(prefs::kNexusWebappUrl,
                                 "https://example.com/agent");

  EXPECT_EQ(GURL("https://example.com/agent"),
            GetResolvedNexusGuestURL(profile.get()));
}

TEST(NexusGuestConfigTest, GetResolvedNexusGuestURLAcceptsLocalhostDevUrls) {
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

TEST(NexusGuestConfigTest, GetResolvedNexusGuestURLRejectsInvalidCustomUrls) {
  std::unique_ptr<TestingProfile> profile = CreateTestingProfileWithNexusPrefs();

  profile->GetPrefs()->SetString(prefs::kNexusWebappUrl,
                                 "http://example.com/not-allowed");
  EXPECT_EQ(GURL(kDefaultNexusGuestURL),
            GetResolvedNexusGuestURL(profile.get()));

  profile->GetPrefs()->SetString(prefs::kNexusWebappUrl, "ftp://localhost/app");
  EXPECT_EQ(GURL(kDefaultNexusGuestURL),
            GetResolvedNexusGuestURL(profile.get()));
}

}  // namespace
}  // namespace nexus
