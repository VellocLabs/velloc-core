// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/features/patch_webui/patch_resources.h"

#include <string_view>

#include "testing/gtest/include/gtest/gtest.h"

namespace custom_browser::patch_webui {
namespace {

// Regression test for the Chromium-149 upgrade break where the settings
// sub-pages (Get Started, Appearance, About) silently stopped applying their
// patches. The settings loader (settings/custom_main.ts) loads each per-page
// module by file name -- `import('./about_patch.js')` etc. -- but the resource
// registrar had been changed to register those modules under their source-tree
// subdirectory (`about/about_patch.js`). Every `chrome://settings/<name>.js`
// import then 404'd, so none of the three patches attached. Registration must
// collapse a nested resource path to its file name.
TEST(PatchResourcesTest, RegistersNestedSettingsModuleByFileName) {
  EXPECT_EQ(PatchResourceRegistrationName("settings/about/about_patch.js"),
            "about_patch.js");
  EXPECT_EQ(
      PatchResourceRegistrationName("settings/appearance/appearance_patch.js"),
      "appearance_patch.js");
  EXPECT_EQ(PatchResourceRegistrationName(
                "settings/get-started/get_started_patch.js"),
            "get_started_patch.js");
  EXPECT_EQ(
      PatchResourceRegistrationName("settings/get-started/get_started_page.js"),
      "get_started_page.js");
}

// The New Tab Page builds icon URLs as `${origin}/${fileName}` (no `icons/`
// segment), so a resource nested under `icons/` must register by file name too.
TEST(PatchResourcesTest, RegistersNestedNewTabPageIconByFileName) {
  EXPECT_EQ(PatchResourceRegistrationName("new-tab-page/icons/ask_logo.svg"),
            "ask_logo.svg");
}

// A resource that already sits directly under its host directory is unchanged.
TEST(PatchResourcesTest, LeavesFlatResourceNameUnchanged) {
  EXPECT_EQ(PatchResourceRegistrationName("settings/custom_main.js"),
            "custom_main.js");
  EXPECT_EQ(PatchResourceRegistrationName("version/version_patch.js"),
            "version_patch.js");
}

// Shared mojom modules have no directory component and pass through verbatim.
TEST(PatchResourcesTest, LeavesPathWithoutSlashUnchanged) {
  EXPECT_EQ(PatchResourceRegistrationName("foo.mojom-webui.js"),
            "foo.mojom-webui.js");
}

}  // namespace
}  // namespace custom_browser::patch_webui
