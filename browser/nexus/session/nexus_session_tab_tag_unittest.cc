// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/session/nexus_session_tab_tag.h"

#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "chrome/test/base/testing_profile.h"
#include "content/public/browser/web_contents.h"
#include "custom_browser/browser/nexus/nexus_test_support.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

class NexusSessionTabTagTest : public ChromeRenderViewHostTestHarness {
 protected:
  // The harness builds a regular TestingProfile, which would otherwise eagerly
  // construct GlicKeyedService and crash without a real ProfileManager. See
  // nexus_test_support.h.
  TestingProfile::TestingFactories GetTestingFactories() const override {
    return test::GlicDisablingTestingFactories();
  }
};

TEST_F(NexusSessionTabTagTest, ReturnsNullWhenNotAttached) {
  EXPECT_EQ(NexusSessionTabTag::SessionKeyFor(web_contents()), nullptr);
}

TEST_F(NexusSessionTabTagTest, AttachThenReadRoundTrips) {
  NexusSessionTabTag::AttachTo(web_contents(), "session-A");

  const std::string* key = NexusSessionTabTag::SessionKeyFor(web_contents());
  ASSERT_NE(key, nullptr);
  EXPECT_EQ(*key, "session-A");
}

TEST_F(NexusSessionTabTagTest, AttachIsIdempotentForSameKey) {
  NexusSessionTabTag::AttachTo(web_contents(), "session-A");
  NexusSessionTabTag* first = NexusSessionTabTag::FromWebContents(web_contents());
  ASSERT_NE(first, nullptr);
  NexusSessionTabTag::AttachTo(web_contents(), "session-A");
  // Same key → no replacement.
  EXPECT_EQ(NexusSessionTabTag::FromWebContents(web_contents()), first);
}

TEST_F(NexusSessionTabTagTest, AttachReplacesWhenKeyDiffers) {
  NexusSessionTabTag::AttachTo(web_contents(), "session-A");
  NexusSessionTabTag::AttachTo(web_contents(), "session-B");
  const std::string* key = NexusSessionTabTag::SessionKeyFor(web_contents());
  ASSERT_NE(key, nullptr);
  // The tag must always be single-valued — re-binding to a different
  // session_key replaces the prior tag rather than stacking.
  EXPECT_EQ(*key, "session-B");
}

TEST_F(NexusSessionTabTagTest, AttachToNullIsNoOp) {
  // Defensive — callers may pass null when racing with tab close.
  NexusSessionTabTag::AttachTo(/*contents=*/nullptr, "session-X");
  EXPECT_EQ(NexusSessionTabTag::SessionKeyFor(/*contents=*/nullptr),
            nullptr);
}

}  // namespace
}  // namespace nexus
