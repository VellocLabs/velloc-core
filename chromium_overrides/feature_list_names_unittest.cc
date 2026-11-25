// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/chromium_overrides/feature_list_names.h"

#include <string>

#include "testing/gtest/include/gtest/gtest.h"

namespace custom_browser {
namespace {

TEST(FeatureListNamesTest, MatchesAWholeName) {
  EXPECT_TRUE(FeatureListNamesFeature("InitialWebUI", "InitialWebUI"));
  EXPECT_TRUE(FeatureListNamesFeature("A,InitialWebUI,B", "InitialWebUI"));
  EXPECT_TRUE(FeatureListNamesFeature("A, InitialWebUI ", "InitialWebUI"));
}

TEST(FeatureListNamesTest, ANameThatOnlyContainsTheFeatureIsNotIt) {
  EXPECT_FALSE(FeatureListNamesFeature("InitialWebUIFoo", "InitialWebUI"));
  EXPECT_FALSE(FeatureListNamesFeature("FooInitialWebUI", "InitialWebUI"));
  EXPECT_FALSE(
      FeatureListNamesFeature("SearchSettingsUpdateV2", "SearchSettingsUpdate"));
  EXPECT_FALSE(FeatureListNamesFeature("", "InitialWebUI"));
}

TEST(FeatureListNamesTest, StudyGroupParamsAndStarAreNotPartOfTheName) {
  EXPECT_TRUE(FeatureListNamesFeature("InitialWebUI<Study", "InitialWebUI"));
  EXPECT_TRUE(
      FeatureListNamesFeature("InitialWebUI<Study.Group", "InitialWebUI"));
  EXPECT_TRUE(FeatureListNamesFeature("InitialWebUI:p/1", "InitialWebUI"));
  EXPECT_TRUE(FeatureListNamesFeature("*InitialWebUI", "InitialWebUI"));
  EXPECT_FALSE(
      FeatureListNamesFeature("InitialWebUIFoo<Study", "InitialWebUI"));
}

TEST(PinnedOffFeaturesTest, AnEmptyListGainsEveryPinnedFeature) {
  const std::string pinned = WithPinnedOffFeatures("");
  for (const char* feature : kPinnedOffFeatureNames) {
    EXPECT_TRUE(FeatureListNamesFeature(pinned, feature)) << feature;
  }
  // Each of these is relied on by name: dropping one brings back the fault
  // it was pinned for.
  EXPECT_TRUE(FeatureListNamesFeature(pinned, "SearchSettingsUpdate"));
  EXPECT_TRUE(FeatureListNamesFeature(pinned, "InitialWebUI"));
  EXPECT_TRUE(FeatureListNamesFeature(pinned, "CloudSyncedFolderChecker"));
}

TEST(PinnedOffFeaturesTest, KeepsWhatTheCallerDisabled) {
  const std::string pinned = WithPinnedOffFeatures("Foo,Bar<Study");
  EXPECT_TRUE(FeatureListNamesFeature(pinned, "Foo"));
  EXPECT_TRUE(FeatureListNamesFeature(pinned, "Bar"));
  EXPECT_TRUE(FeatureListNamesFeature(pinned, "CloudSyncedFolderChecker"));
}

TEST(PinnedOffFeaturesTest, AFeatureAlreadyNamedIsNotAddedAgain) {
  const std::string once = WithPinnedOffFeatures("CloudSyncedFolderChecker");
  EXPECT_EQ(once.find("CloudSyncedFolderChecker"),
            once.rfind("CloudSyncedFolderChecker"));
  // A second pass has nothing left to add.
  EXPECT_EQ(once, WithPinnedOffFeatures(once));
}

}  // namespace
}  // namespace custom_browser
