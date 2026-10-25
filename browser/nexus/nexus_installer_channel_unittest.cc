// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/nexus_installer_channel.h"

#include <optional>
#include <string>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/test/task_environment.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/testing_pref_service.h"
#include "custom_browser/browser/nexus/api/llm/llm_pref_names.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

TEST(NexusInstallerChannelTest, BetaNamesTheBetaOrigin) {
  EXPECT_EQ(WebappUrlForInstallerChannel("beta"),
            std::string(kBetaChannelWebappUrl));
  EXPECT_EQ(WebappUrlForInstallerChannel(" BETA\r\n"),
            std::string(kBetaChannelWebappUrl));
}

TEST(NexusInstallerChannelTest, OtherNamesSelectNothing) {
  EXPECT_EQ(WebappUrlForInstallerChannel(""), std::nullopt);
  EXPECT_EQ(WebappUrlForInstallerChannel("stable"), std::nullopt);
  EXPECT_EQ(WebappUrlForInstallerChannel("betas"), std::nullopt);
  EXPECT_EQ(WebappUrlForInstallerChannel("nightly"), std::nullopt);
}

class NexusInstallerChannelMarkerTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(dir_.CreateUniqueTempDir());
    prefs_.registry()->RegisterStringPref(prefs::kNexusWebappUrl,
                                          std::string());
  }

  void TearDown() override {
    // Drop anything a test left pending; the pending marker is process-wide.
    ApplyPendingInstallerChannel(&prefs_);
    task_environment_.RunUntilIdle();
  }

  base::FilePath MarkerPath() const {
    return dir_.GetPath().Append(kInstallerChannelFileName);
  }

  void WriteMarker(const std::string& content) {
    ASSERT_TRUE(base::WriteFile(MarkerPath(), content));
  }

  base::test::TaskEnvironment task_environment_;
  base::ScopedTempDir dir_;
  TestingPrefServiceSimple prefs_;
};

TEST_F(NexusInstallerChannelMarkerTest, NoMarkerLeavesThePrefAlone) {
  prefs_.SetString(prefs::kNexusWebappUrl, "https://mirror.example/");
  LoadPendingInstallerChannel(dir_.GetPath());
  EXPECT_FALSE(ApplyPendingInstallerChannel(&prefs_));
  EXPECT_EQ(prefs_.GetString(prefs::kNexusWebappUrl),
            "https://mirror.example/");
}

TEST_F(NexusInstallerChannelMarkerTest, BetaMarkerSelectsBetaOnce) {
  WriteMarker("beta");
  LoadPendingInstallerChannel(dir_.GetPath());
  EXPECT_TRUE(ApplyPendingInstallerChannel(&prefs_));
  EXPECT_EQ(prefs_.GetString(prefs::kNexusWebappUrl), kBetaChannelWebappUrl);
  task_environment_.RunUntilIdle();
  EXPECT_FALSE(base::PathExists(MarkerPath()));

  // A later switch back to stable survives the next launch.
  prefs_.SetString(prefs::kNexusWebappUrl, std::string());
  LoadPendingInstallerChannel(dir_.GetPath());
  EXPECT_FALSE(ApplyPendingInstallerChannel(&prefs_));
  EXPECT_EQ(prefs_.GetString(prefs::kNexusWebappUrl), std::string());
}

TEST_F(NexusInstallerChannelMarkerTest, BetaMarkerOverridesACustomUrl) {
  prefs_.SetString(prefs::kNexusWebappUrl, "https://mirror.example/");
  WriteMarker("Beta\r\n");
  LoadPendingInstallerChannel(dir_.GetPath());
  EXPECT_TRUE(ApplyPendingInstallerChannel(&prefs_));
  EXPECT_EQ(prefs_.GetString(prefs::kNexusWebappUrl), kBetaChannelWebappUrl);
}

TEST_F(NexusInstallerChannelMarkerTest, MarkerStaysUntilAppliedToPrefs) {
  WriteMarker("beta");
  LoadPendingInstallerChannel(dir_.GetPath());
  EXPECT_FALSE(ApplyPendingInstallerChannel(nullptr));
  task_environment_.RunUntilIdle();
  EXPECT_TRUE(base::PathExists(MarkerPath()));

  EXPECT_TRUE(ApplyPendingInstallerChannel(&prefs_));
  EXPECT_FALSE(ApplyPendingInstallerChannel(&prefs_));
}

TEST_F(NexusInstallerChannelMarkerTest, UnknownMarkerIsDiscarded) {
  WriteMarker("nightly");
  LoadPendingInstallerChannel(dir_.GetPath());
  EXPECT_FALSE(ApplyPendingInstallerChannel(&prefs_));
  EXPECT_EQ(prefs_.GetString(prefs::kNexusWebappUrl), std::string());
  task_environment_.RunUntilIdle();
  EXPECT_FALSE(base::PathExists(MarkerPath()));
}

}  // namespace
}  // namespace nexus
