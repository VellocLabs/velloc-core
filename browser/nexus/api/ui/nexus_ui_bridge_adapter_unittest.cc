// Copyright 2026 The Custom Browser Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/ui/nexus_ui_bridge_adapter.h"

#include <optional>
#include <string>

#include "base/strings/utf_string_conversions.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "custom_browser/common/branding_constants.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/base/clipboard/clipboard.h"
#include "ui/base/clipboard/clipboard_buffer.h"
#include "ui/base/clipboard/test/test_clipboard.h"
#include "ui/gfx/image/image.h"
#include "ui/message_center/public/cpp/notification.h"

namespace nexus {
namespace {

// Regression guard for the turn-end OS-notification HEADER icon.
//
// HandleShowNotificationOnUI sets the notification's small_image (the icon next
// to the product name in the toast header) to the branded Nexus logo. Without it,
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

// Regression guard for the toast HEADER text ("<name> • now" on Windows) and
// the bold-line fallback: both must carry the user-visible product name from
// branding_constants.h, never the internal "Nexus" codename, which was
// previously hardcoded here. Delegate is null — only
// NotificationDisplayServiceImpl::Display requires one, not construction.
TEST(NexusNotificationBrandingTest, ToastHeaderUsesBrandedProductName) {
  const std::u16string product_name =
      base::ASCIIToUTF16(custom_browser::kCustomBrowserProductName);
  ASSERT_NE(product_name, u"Nexus");

  const message_center::Notification with_title = BuildNexusToastNotification(
      "My session", "Velloc is waiting for your input", "turn_complete",
      /*delegate=*/nullptr);
  EXPECT_EQ(with_title.display_source(), product_name);
  EXPECT_EQ(with_title.title(), u"My session");
  EXPECT_EQ(with_title.id(), "nexus.turn_complete");

  // No session title yet → the bold line falls back to the product name.
  const message_center::Notification without_title =
      BuildNexusToastNotification(std::nullopt, "message", std::nullopt,
                                  /*delegate=*/nullptr);
  EXPECT_EQ(without_title.display_source(), product_name);
  EXPECT_EQ(without_title.title(), product_name);
  EXPECT_EQ(without_title.id(), "nexus.default");
}

// ui.writeClipboard (a mod's `$.ui.copy`): the text lands on the copy/paste
// clipboard, read back as written — non-ASCII included — and the callback says
// so. A later write replaces it.
TEST(NexusWriteClipboardTest, WritesTextAndConfirmsByReadBack) {
  base::test::TaskEnvironment task_environment;
  ui::TestClipboard::CreateForCurrentThread();

  base::test::TestFuture<bool> first;
  WriteNexusClipboardText("copied by a mod \xE2\x9C\x93", first.GetCallback());
  EXPECT_TRUE(first.Get());

  base::test::TestFuture<std::u16string> held;
  ui::Clipboard::GetForCurrentThread()->ReadText(
      ui::ClipboardBuffer::kCopyPaste, std::nullopt, held.GetCallback());
  EXPECT_EQ(held.Get(), u"copied by a mod \u2713");

  base::test::TestFuture<bool> second;
  WriteNexusClipboardText("second", second.GetCallback());
  EXPECT_TRUE(second.Get());
  base::test::TestFuture<std::u16string> again;
  ui::Clipboard::GetForCurrentThread()->ReadText(
      ui::ClipboardBuffer::kCopyPaste, std::nullopt, again.GetCallback());
  EXPECT_EQ(again.Get(), u"second");

  ui::Clipboard::DestroyClipboardForCurrentThread();
}

}  // namespace
}  // namespace nexus
