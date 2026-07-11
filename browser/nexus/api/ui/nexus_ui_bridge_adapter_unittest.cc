// Copyright 2026 The Custom Browser Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/ui/nexus_ui_bridge_adapter.h"

#include "testing/gtest/include/gtest/gtest.h"
#include "ui/gfx/image/image.h"

namespace nexus {
namespace {

// Regression guard for the turn-end OS-notification HEADER icon.
//
// HandleShowNotificationOnUI sets the notification's small_image (the icon next
// to "Nexus" in the toast header) to the branded Nexus logo. Without it,
// message_center falls back to its default kProductIcon — the unbranded
// Chromium spiral. The main icon() (the large right-side image) is left empty
// on purpose. This asserts GetNexusNotificationHeaderIcon() resolves
// IDR_PRODUCT_LOGO_64 to a real, non-empty image, so a regression back to an
// empty/default header icon fails here.
//
// Lives in the heavier custom_browser_nexus_browser_unittests binary (not the
// lite one) because GetImageNamed needs the chrome resource bundle, which
// ChromeUnitTestSuite (//chrome/test:test_support_unit) loads for this binary.
TEST(NexusNotificationIconTest, HeaderIconResolvesToNonEmptyBrandedLogo) {
  const gfx::Image icon = GetNexusNotificationHeaderIcon();
  EXPECT_FALSE(icon.IsEmpty());
  EXPECT_GT(icon.Width(), 0);
  EXPECT_GT(icon.Height(), 0);
}

}  // namespace
}  // namespace nexus
