// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include <string>

#include "build/build_config.h"
#include "custom_browser/chromium_overrides/feature_list_names.h"
#include "testing/gtest/include/gtest/gtest.h"

#if BUILDFLAG(IS_WIN)
#include "chrome/browser/win/cloud_synced_folder_checker.h"
#endif

namespace custom_browser {
namespace {

#if BUILDFLAG(IS_WIN)
// A pin is a name on --disable-features. An upstream rename would leave the
// pin naming nothing and the feature back on, with no build error.
TEST(FeatureListPinsTest, TheCloudSyncedFolderCheckerPinNamesTheFeature) {
  EXPECT_TRUE(FeatureListNamesFeature(
      WithPinnedOffFeatures(""),
      cloud_synced_folder_checker::features::kCloudSyncedFolderChecker.name));
}
#endif

}  // namespace
}  // namespace custom_browser
