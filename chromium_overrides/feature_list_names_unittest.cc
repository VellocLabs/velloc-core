// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/chromium_overrides/feature_list_names.h"

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

}  // namespace
}  // namespace custom_browser
