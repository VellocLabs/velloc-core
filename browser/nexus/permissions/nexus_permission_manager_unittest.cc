// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/permissions/nexus_permission_manager.h"

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace nexus {
namespace {

url::Origin MakeOrigin(const std::string& url) {
  return url::Origin::Create(GURL(url));
}

// Captures the boolean passed to a PermissionCallback for assertions.
class CallbackCapture {
 public:
  NexusPermissionManager::PermissionCallback AsCallback() {
    return base::BindOnce(&CallbackCapture::OnCalled, base::Unretained(this));
  }

  bool called() const { return called_; }
  bool granted() const { return granted_; }

 private:
  void OnCalled(bool granted) {
    called_ = true;
    granted_ = granted;
  }

  bool called_ = false;
  bool granted_ = false;
};

// -----------------------------------------------------------------------------
// ParseCapability / CapabilityToString
// -----------------------------------------------------------------------------

TEST(NexusCapabilityTest, ParseCapabilityKnownValues) {
  EXPECT_EQ(ParseCapability("file-system"), NexusCapability::kFileSystem);
  EXPECT_EQ(ParseCapability("clipboard"), NexusCapability::kClipboard);
  EXPECT_EQ(ParseCapability("browser-data"), NexusCapability::kBrowserData);
  EXPECT_EQ(ParseCapability("notifications"),
            NexusCapability::kNotifications);
}

TEST(NexusCapabilityTest, ParseCapabilityUnknownReturnsNullopt) {
  EXPECT_FALSE(ParseCapability("").has_value());
  EXPECT_FALSE(ParseCapability("File-System").has_value())
      << "capability strings are case-sensitive";
  EXPECT_FALSE(ParseCapability("file_system").has_value())
      << "capability strings use hyphens, not underscores";
  EXPECT_FALSE(ParseCapability("camera").has_value());
}

TEST(NexusCapabilityTest, CapabilityToStringRoundTripsThroughParse) {
  for (auto cap : {NexusCapability::kFileSystem, NexusCapability::kClipboard,
                   NexusCapability::kBrowserData,
                   NexusCapability::kNotifications}) {
    const std::string str = CapabilityToString(cap);
    EXPECT_FALSE(str.empty()) << "CapabilityToString returned empty";
    auto parsed = ParseCapability(str);
    ASSERT_TRUE(parsed.has_value())
        << "round-trip failed: CapabilityToString gave '" << str
        << "' which ParseCapability rejects";
    EXPECT_EQ(*parsed, cap);
  }
}

// -----------------------------------------------------------------------------
// CheckPermission — initial state
// -----------------------------------------------------------------------------

TEST(NexusPermissionManagerTest, UnknownOriginReturnsNotAsked) {
  NexusPermissionManager mgr;
  EXPECT_EQ(mgr.CheckPermission(MakeOrigin("https://example.com"),
                                NexusCapability::kFileSystem),
            PermissionStatus::kNotAsked);
}

// -----------------------------------------------------------------------------
// GrantPermission / RevokePermission — direct transitions
// -----------------------------------------------------------------------------

TEST(NexusPermissionManagerTest, GrantPermissionFlipsToGranted) {
  NexusPermissionManager mgr;
  const url::Origin origin = MakeOrigin("https://example.com");

  mgr.GrantPermission(origin, NexusCapability::kClipboard);

  EXPECT_EQ(mgr.CheckPermission(origin, NexusCapability::kClipboard),
            PermissionStatus::kGranted);
}

TEST(NexusPermissionManagerTest, RevokePermissionFlipsToDenied) {
  NexusPermissionManager mgr;
  const url::Origin origin = MakeOrigin("https://example.com");

  mgr.RevokePermission(origin, NexusCapability::kClipboard);

  EXPECT_EQ(mgr.CheckPermission(origin, NexusCapability::kClipboard),
            PermissionStatus::kDenied);
}

TEST(NexusPermissionManagerTest, GrantAfterRevokeReversesState) {
  // data-model.md: DENIED → GRANTED (user re-grants in settings).
  NexusPermissionManager mgr;
  const url::Origin origin = MakeOrigin("https://example.com");

  mgr.RevokePermission(origin, NexusCapability::kFileSystem);
  mgr.GrantPermission(origin, NexusCapability::kFileSystem);

  EXPECT_EQ(mgr.CheckPermission(origin, NexusCapability::kFileSystem),
            PermissionStatus::kGranted);
}

TEST(NexusPermissionManagerTest, RevokeAfterGrantFlipsToDenied) {
  // data-model.md: GRANTED → DENIED (user revokes in settings).
  NexusPermissionManager mgr;
  const url::Origin origin = MakeOrigin("https://example.com");

  mgr.GrantPermission(origin, NexusCapability::kFileSystem);
  mgr.RevokePermission(origin, NexusCapability::kFileSystem);

  EXPECT_EQ(mgr.CheckPermission(origin, NexusCapability::kFileSystem),
            PermissionStatus::kDenied);
}

// -----------------------------------------------------------------------------
// Per-origin isolation (FR-019a)
// -----------------------------------------------------------------------------

TEST(NexusPermissionManagerTest, GrantsAreIsolatedAcrossOrigins) {
  NexusPermissionManager mgr;
  const url::Origin a = MakeOrigin("https://a.test");
  const url::Origin b = MakeOrigin("https://b.test");

  mgr.GrantPermission(a, NexusCapability::kClipboard);

  EXPECT_EQ(mgr.CheckPermission(a, NexusCapability::kClipboard),
            PermissionStatus::kGranted);
  EXPECT_EQ(mgr.CheckPermission(b, NexusCapability::kClipboard),
            PermissionStatus::kNotAsked)
      << "granting to a.test must not leak to b.test";
}

TEST(NexusPermissionManagerTest, GrantsAreIsolatedAcrossCapabilities) {
  NexusPermissionManager mgr;
  const url::Origin origin = MakeOrigin("https://example.com");

  mgr.GrantPermission(origin, NexusCapability::kClipboard);

  EXPECT_EQ(mgr.CheckPermission(origin, NexusCapability::kClipboard),
            PermissionStatus::kGranted);
  EXPECT_EQ(mgr.CheckPermission(origin, NexusCapability::kFileSystem),
            PermissionStatus::kNotAsked)
      << "granting clipboard must not imply file-system";
}

// -----------------------------------------------------------------------------
// RequestPermission
// -----------------------------------------------------------------------------

TEST(NexusPermissionManagerTest, RequestPermissionPassesThroughGrantedState) {
  NexusPermissionManager mgr;
  const url::Origin origin = MakeOrigin("https://example.com");
  mgr.GrantPermission(origin, NexusCapability::kClipboard);

  CallbackCapture cb;
  mgr.RequestPermission(origin, NexusCapability::kClipboard, cb.AsCallback());

  EXPECT_TRUE(cb.called());
  EXPECT_TRUE(cb.granted());
  // State must remain GRANTED — request on a granted permission is a
  // no-op on the stored state.
  EXPECT_EQ(mgr.CheckPermission(origin, NexusCapability::kClipboard),
            PermissionStatus::kGranted);
}

TEST(NexusPermissionManagerTest, RequestPermissionPassesThroughDeniedState) {
  NexusPermissionManager mgr;
  const url::Origin origin = MakeOrigin("https://example.com");
  mgr.RevokePermission(origin, NexusCapability::kClipboard);

  CallbackCapture cb;
  mgr.RequestPermission(origin, NexusCapability::kClipboard, cb.AsCallback());

  EXPECT_TRUE(cb.called());
  EXPECT_FALSE(cb.granted())
      << "DENIED must not escalate to GRANTED via RequestPermission";
  EXPECT_EQ(mgr.CheckPermission(origin, NexusCapability::kClipboard),
            PermissionStatus::kDenied);
}

TEST(NexusPermissionManagerTest, RequestPermissionAutoGrantsWhenNotAsked) {
  // Current behavior per the TODO in nexus_permission_manager.cc:
  // "Auto-grant to unblock development. This will be replaced with a
  // proper permission bubble UI in a follow-up task."
  // This test is a regression guard for that documented transitional
  // behaviour — if a real prompt UI lands, this assertion will flip and
  // the test should be updated to drive the prompt.
  NexusPermissionManager mgr;
  const url::Origin origin = MakeOrigin("https://example.com");

  CallbackCapture cb;
  mgr.RequestPermission(origin, NexusCapability::kFileSystem,
                        cb.AsCallback());

  EXPECT_TRUE(cb.called());
  EXPECT_TRUE(cb.granted());
  EXPECT_EQ(mgr.CheckPermission(origin, NexusCapability::kFileSystem),
            PermissionStatus::kGranted);
}

// -----------------------------------------------------------------------------
// GetPermissions
// -----------------------------------------------------------------------------

TEST(NexusPermissionManagerTest, GetPermissionsReturnsOnlyMatchingOrigin) {
  NexusPermissionManager mgr;
  const url::Origin a = MakeOrigin("https://a.test");
  const url::Origin b = MakeOrigin("https://b.test");
  mgr.GrantPermission(a, NexusCapability::kClipboard);
  mgr.GrantPermission(a, NexusCapability::kNotifications);
  mgr.RevokePermission(a, NexusCapability::kFileSystem);
  mgr.GrantPermission(b, NexusCapability::kFileSystem);

  auto perms_a = mgr.GetPermissions(a);
  ASSERT_EQ(perms_a.size(), 3u);
  EXPECT_EQ(perms_a[NexusCapability::kClipboard], PermissionStatus::kGranted);
  EXPECT_EQ(perms_a[NexusCapability::kNotifications],
            PermissionStatus::kGranted);
  EXPECT_EQ(perms_a[NexusCapability::kFileSystem], PermissionStatus::kDenied);

  auto perms_b = mgr.GetPermissions(b);
  ASSERT_EQ(perms_b.size(), 1u);
  EXPECT_EQ(perms_b[NexusCapability::kFileSystem],
            PermissionStatus::kGranted);
}

TEST(NexusPermissionManagerTest, GetPermissionsEmptyForUnknownOrigin) {
  NexusPermissionManager mgr;
  EXPECT_TRUE(
      mgr.GetPermissions(MakeOrigin("https://example.com")).empty());
}

// -----------------------------------------------------------------------------
// ClearPermissions
// -----------------------------------------------------------------------------

TEST(NexusPermissionManagerTest, ClearPermissionsWipesMatchingOriginOnly) {
  NexusPermissionManager mgr;
  const url::Origin a = MakeOrigin("https://a.test");
  const url::Origin b = MakeOrigin("https://b.test");
  mgr.GrantPermission(a, NexusCapability::kClipboard);
  mgr.GrantPermission(a, NexusCapability::kFileSystem);
  mgr.GrantPermission(b, NexusCapability::kClipboard);

  mgr.ClearPermissions(a);

  // a's grants return to NotAsked (map entry fully removed).
  EXPECT_EQ(mgr.CheckPermission(a, NexusCapability::kClipboard),
            PermissionStatus::kNotAsked);
  EXPECT_EQ(mgr.CheckPermission(a, NexusCapability::kFileSystem),
            PermissionStatus::kNotAsked);
  EXPECT_TRUE(mgr.GetPermissions(a).empty());

  // b's grant must be untouched.
  EXPECT_EQ(mgr.CheckPermission(b, NexusCapability::kClipboard),
            PermissionStatus::kGranted);
}

TEST(NexusPermissionManagerTest, ClearPermissionsOnUnknownOriginIsSilent) {
  NexusPermissionManager mgr;
  // Must not crash / DCHECK.
  mgr.ClearPermissions(MakeOrigin("https://never-touched.test"));
}

}  // namespace
}  // namespace nexus
