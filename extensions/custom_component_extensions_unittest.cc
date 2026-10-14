// Copyright 2026 VellocLabs
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/extensions/custom_component_extensions.h"

#include "base/files/file_path.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/testing_pref_service.h"
#include "custom_browser/common/pref_names.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace custom_browser {
namespace {

// A stand-in entry: the pref plumbing never looks at the manifest resource,
// so the fields that would need a ResourceBundle can stay empty.
constexpr CustomComponentExtension kTestExtensionEnabledByDefault = {
    "test_on", 0, FILE_PATH_LITERAL("custom_extensions/test_on"), false, true};
constexpr CustomComponentExtension kTestExtensionDisabledByDefault = {
    "test_off", 0, FILE_PATH_LITERAL("custom_extensions/test_off"), false,
    false};

class CustomComponentExtensionsTest : public testing::Test {
 protected:
  void SetUp() override {
    prefs_.registry()->RegisterDictionaryPref(
        kCustomComponentExtensionsPrefPath);
  }

  TestingPrefServiceSimple prefs_;
};

// The point of this change: nothing we bundle turns itself on. A new built-in
// extension that ships enabled has to change this test deliberately.
TEST(CustomComponentExtensionsBundleTest, EveryBundledExtensionDefaultsToOff) {
  for (const auto& extension : GetCustomComponentExtensions()) {
    EXPECT_FALSE(extension.enabled_by_default)
        << "bundled extension '" << extension.name
        << "' is enabled by default; built-in extensions are opt-in";
  }
}

TEST_F(CustomComponentExtensionsTest, UsesDefaultWhenProfileHasNoPref) {
  EXPECT_TRUE(IsCustomComponentExtensionEnabled(
      &prefs_, kTestExtensionEnabledByDefault));
  EXPECT_FALSE(IsCustomComponentExtensionEnabled(
      &prefs_, kTestExtensionDisabledByDefault));
}

TEST_F(CustomComponentExtensionsTest, UsesDefaultWithoutPrefService) {
  EXPECT_TRUE(IsCustomComponentExtensionEnabled(
      nullptr, kTestExtensionEnabledByDefault));
  EXPECT_FALSE(IsCustomComponentExtensionEnabled(
      nullptr, kTestExtensionDisabledByDefault));
}

// Once the user has toggled it, the stored pref outranks the shipped default
// in both directions — flipping a default must not silently re-enable an
// extension somebody turned off, nor turn off one they turned on.
TEST_F(CustomComponentExtensionsTest, StoredPrefOverridesDefault) {
  SetCustomComponentExtensionEnabled(&prefs_, kTestExtensionEnabledByDefault,
                                     false);
  SetCustomComponentExtensionEnabled(&prefs_, kTestExtensionDisabledByDefault,
                                     true);

  EXPECT_FALSE(IsCustomComponentExtensionEnabled(
      &prefs_, kTestExtensionEnabledByDefault));
  EXPECT_TRUE(IsCustomComponentExtensionEnabled(
      &prefs_, kTestExtensionDisabledByDefault));
}

}  // namespace
}  // namespace custom_browser
