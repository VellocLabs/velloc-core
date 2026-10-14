// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/upload/nexus_pending_agent_notice.h"

#include <vector>

#include "base/memory/scoped_refptr.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "chrome/test/base/testing_profile.h"
#include "content/public/browser/file_select_listener.h"
#include "content/public/browser/web_contents.h"
#include "custom_browser/browser/nexus/nexus_test_support.h"
#include "custom_browser/browser/nexus/session/nexus_session_tab_tag.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/mojom/choosers/file_chooser.mojom.h"

namespace nexus {
namespace {

// Records which terminal callback the chooser interceptor invoked.
class FakeFileSelectListener : public content::FileSelectListener {
 public:
  bool canceled = false;
  bool selected = false;

  void FileSelected(std::vector<blink::mojom::FileChooserFileInfoPtr> /*files*/,
                    const base::FilePath& /*base_dir*/,
                    blink::mojom::FileChooserParams::Mode /*mode*/) override {
    selected = true;
  }
  void FileSelectionCanceled() override { canceled = true; }

 protected:
  ~FakeFileSelectListener() override = default;
};

blink::mojom::FileChooserParams ParamsForMode(
    blink::mojom::FileChooserParams::Mode mode) {
  blink::mojom::FileChooserParams params;
  params.mode = mode;
  return params;
}

// ---- AgentNotice::ToDict (pure, no harness) --------------------------------

TEST(AgentNoticeToDictTest, IncludesRecommendedToolWhenSet) {
  AgentNotice n;
  n.type = "native_dialog_suppressed";
  n.dialog = "open_file";
  n.recommended_tool = "BrowserUpload";
  n.message = "msg";

  base::DictValue d = n.ToDict();
  ASSERT_NE(d.FindString("type"), nullptr);
  EXPECT_EQ(*d.FindString("type"), "native_dialog_suppressed");
  EXPECT_EQ(*d.FindString("dialog"), "open_file");
  EXPECT_EQ(*d.FindString("recommended_tool"), "BrowserUpload");
  EXPECT_EQ(*d.FindString("message"), "msg");
}

TEST(AgentNoticeToDictTest, OmitsEmptyOptionalFields) {
  AgentNotice n;
  n.type = "native_dialog_suppressed";
  n.dialog = "directory";
  n.message = "msg";

  base::DictValue d = n.ToDict();
  // recommended_tool empty → key omitted; dialog present → key kept.
  EXPECT_EQ(d.FindString("recommended_tool"), nullptr);
  EXPECT_NE(d.FindString("dialog"), nullptr);
  EXPECT_NE(d.FindString("message"), nullptr);
}

// ---- Queue + chooser suppression (WebContents harness) ---------------------

class NexusPendingAgentNoticeTest : public ChromeRenderViewHostTestHarness {
 protected:
  // Same Glic-disabling factories the tab-tag test uses — a plain
  // TestingProfile would eagerly build GlicKeyedService and crash.
  TestingProfile::TestingFactories GetTestingFactories() const override {
    return test::GlicDisablingTestingFactories();
  }
};

TEST_F(NexusPendingAgentNoticeTest, PushThenTakeAllRoundTripsAndIsOneShot) {
  AgentNotice n;
  n.type = "t";
  n.message = "m";
  NexusPendingAgentNotice::Push(web_contents(), n);

  auto* q = NexusPendingAgentNotice::FromWebContents(web_contents());
  ASSERT_NE(q, nullptr);
  std::vector<AgentNotice> out = q->TakeAll();
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].type, "t");
  EXPECT_EQ(out[0].message, "m");
  // Drained once → gone.
  EXPECT_TRUE(q->TakeAll().empty());
}

TEST_F(NexusPendingAgentNoticeTest, PushNullContentsIsNoOp) {
  AgentNotice n;
  n.type = "t";
  NexusPendingAgentNotice::Push(/*contents=*/nullptr, n);  // must not crash
  SUCCEED();
}

TEST_F(NexusPendingAgentNoticeTest, DropsOldestPastCap) {
  for (int i = 0; i < 20; ++i) {
    AgentNotice n;
    n.type = base::NumberToString(i);
    NexusPendingAgentNotice::Push(web_contents(), n);
  }
  auto* q = NexusPendingAgentNotice::FromWebContents(web_contents());
  ASSERT_NE(q, nullptr);
  std::vector<AgentNotice> out = q->TakeAll();
  ASSERT_EQ(out.size(), NexusPendingAgentNotice::kMaxNotices);
  // Pushed 0..19, cap 16 → oldest 0..3 dropped; window is 4..19.
  EXPECT_EQ(out.front().type, "4");
  EXPECT_EQ(out.back().type, "19");
}

TEST_F(NexusPendingAgentNoticeTest, SuppressReturnsFalseOnUntaggedTab) {
  auto fake = base::MakeRefCounted<FakeFileSelectListener>();
  scoped_refptr<content::FileSelectListener> listener = fake;
  auto params = ParamsForMode(blink::mojom::FileChooserParams::Mode::kOpen);

  EXPECT_FALSE(NexusPendingAgentNotice::SuppressChooserIfAgentTab(
      web_contents()->GetPrimaryMainFrame(), listener, params));
  // User tab: interceptor must not have touched the listener or queued a notice.
  EXPECT_FALSE(fake->canceled);
  EXPECT_EQ(NexusPendingAgentNotice::FromWebContents(web_contents()), nullptr);
}

TEST_F(NexusPendingAgentNoticeTest, SuppressCancelsAndNotifiesOnAgentTab) {
  NexusSessionTabTag::AttachTo(web_contents(), "session-A");
  auto fake = base::MakeRefCounted<FakeFileSelectListener>();
  scoped_refptr<content::FileSelectListener> listener = fake;
  auto params = ParamsForMode(blink::mojom::FileChooserParams::Mode::kOpen);

  EXPECT_TRUE(NexusPendingAgentNotice::SuppressChooserIfAgentTab(
      web_contents()->GetPrimaryMainFrame(), listener, params));
  EXPECT_TRUE(fake->canceled);
  EXPECT_FALSE(fake->selected);

  auto* q = NexusPendingAgentNotice::FromWebContents(web_contents());
  ASSERT_NE(q, nullptr);
  std::vector<AgentNotice> out = q->TakeAll();
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].type, "native_dialog_suppressed");
  EXPECT_EQ(out[0].dialog, "open_file");
  EXPECT_EQ(out[0].recommended_tool, "BrowserUpload");
}

TEST_F(NexusPendingAgentNoticeTest, SuppressFolderModeHasNoRecommendedTool) {
  NexusSessionTabTag::AttachTo(web_contents(), "session-A");
  auto fake = base::MakeRefCounted<FakeFileSelectListener>();
  scoped_refptr<content::FileSelectListener> listener = fake;
  auto params =
      ParamsForMode(blink::mojom::FileChooserParams::Mode::kUploadFolder);

  EXPECT_TRUE(NexusPendingAgentNotice::SuppressChooserIfAgentTab(
      web_contents()->GetPrimaryMainFrame(), listener, params));

  auto* q = NexusPendingAgentNotice::FromWebContents(web_contents());
  ASSERT_NE(q, nullptr);
  std::vector<AgentNotice> out = q->TakeAll();
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].dialog, "directory");
  EXPECT_TRUE(out[0].recommended_tool.empty());
}

}  // namespace
}  // namespace nexus
