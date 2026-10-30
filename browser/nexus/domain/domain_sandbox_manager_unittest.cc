// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/domain/domain_sandbox_manager.h"

#include <algorithm>

#include "base/files/file_path.h"
#include "base/time/time.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace nexus {
namespace {

constexpr char kProfileDir[] = "C:/fake/profile";

url::Origin MakeOrigin(const std::string& url) {
  return url::Origin::Create(GURL(url));
}

base::FilePath ProfilePath() {
  return base::FilePath::FromASCII(kProfileDir);
}

TEST(DomainSandboxManagerTest, NoActiveDomainInitially) {
  DomainSandboxManager mgr(ProfilePath());
  EXPECT_EQ(mgr.GetActiveSandbox(), nullptr);
  EXPECT_TRUE(mgr.GetActiveDomain().opaque());
  EXPECT_TRUE(mgr.GetAllDomains().empty());
}

TEST(DomainSandboxManagerTest, GetOrCreateSandboxCreatesNewSandbox) {
  DomainSandboxManager mgr(ProfilePath());
  const url::Origin origin = MakeOrigin("https://example.com");

  DomainSandbox* sandbox = mgr.GetOrCreateSandbox(origin);
  ASSERT_NE(sandbox, nullptr);
  EXPECT_EQ(sandbox->origin, origin);
  EXPECT_FALSE(sandbox->first_used.is_null());
  EXPECT_EQ(sandbox->first_used, sandbox->last_used);
  EXPECT_FALSE(sandbox->storage_path.empty());
  EXPECT_TRUE(mgr.HasSandbox(origin));
}

TEST(DomainSandboxManagerTest, GetOrCreateSandboxReturnsExistingSandbox) {
  DomainSandboxManager mgr(ProfilePath());
  const url::Origin origin = MakeOrigin("https://example.com");

  DomainSandbox* first = mgr.GetOrCreateSandbox(origin);
  const base::Time first_used = first->first_used;

  // Second call must return the same sandbox instance — pointer equality is
  // part of the contract: "returned pointer is valid for the lifetime of
  // this service".
  DomainSandbox* second = mgr.GetOrCreateSandbox(origin);
  EXPECT_EQ(first, second);
  EXPECT_EQ(second->first_used, first_used);  // first_used must not change.
  EXPECT_GE(second->last_used, first_used);   // last_used is refreshed.
}

TEST(DomainSandboxManagerTest, DifferentOriginsGetDifferentSandboxes) {
  DomainSandboxManager mgr(ProfilePath());
  DomainSandbox* a = mgr.GetOrCreateSandbox(MakeOrigin("https://a.test"));
  DomainSandbox* b = mgr.GetOrCreateSandbox(MakeOrigin("https://b.test"));

  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_NE(a, b);
  EXPECT_NE(a->origin, b->origin);
  // Storage paths must be unique per-origin (FR-018 data isolation).
  EXPECT_NE(a->storage_path, b->storage_path);
}

TEST(DomainSandboxManagerTest, StoragePathIsUnderProfileSubdir) {
  DomainSandboxManager mgr(ProfilePath());
  DomainSandbox* sandbox =
      mgr.GetOrCreateSandbox(MakeOrigin("https://example.com"));

  // Path should live under <profile>/AgentDomainSandboxes/<hash>.
  const base::FilePath sandboxes_dir =
      ProfilePath().AppendASCII("AgentDomainSandboxes");
  EXPECT_TRUE(sandboxes_dir.IsParent(sandbox->storage_path))
      << "storage_path=" << sandbox->storage_path.value()
      << " expected under " << sandboxes_dir.value();
}

TEST(DomainSandboxManagerTest, StoragePathDeterministicForSameOrigin) {
  const url::Origin origin = MakeOrigin("https://example.com");
  DomainSandboxManager mgr1(ProfilePath());
  DomainSandboxManager mgr2(ProfilePath());
  EXPECT_EQ(mgr1.GetOrCreateSandbox(origin)->storage_path,
            mgr2.GetOrCreateSandbox(origin)->storage_path);
}

TEST(DomainSandboxManagerTest, SetActiveDomainCreatesSandboxIfMissing) {
  DomainSandboxManager mgr(ProfilePath());
  const url::Origin origin = MakeOrigin("https://example.com");
  EXPECT_FALSE(mgr.HasSandbox(origin));

  mgr.SetActiveDomain(origin);

  EXPECT_TRUE(mgr.HasSandbox(origin));
  EXPECT_EQ(mgr.GetActiveDomain(), origin);
  ASSERT_NE(mgr.GetActiveSandbox(), nullptr);
  EXPECT_EQ(mgr.GetActiveSandbox()->origin, origin);
}

TEST(DomainSandboxManagerTest, SwitchingActiveDomainPreservesBothSandboxes) {
  // FR-018b: switching domains presents a clean state.
  // FR-018c: switching back restores all data.
  DomainSandboxManager mgr(ProfilePath());
  const url::Origin a = MakeOrigin("https://a.test");
  const url::Origin b = MakeOrigin("https://b.test");

  mgr.SetActiveDomain(a);
  DomainSandbox* sandbox_a = mgr.GetActiveSandbox();
  ASSERT_NE(sandbox_a, nullptr);

  mgr.SetActiveDomain(b);
  EXPECT_EQ(mgr.GetActiveDomain(), b);
  DomainSandbox* sandbox_b = mgr.GetActiveSandbox();
  ASSERT_NE(sandbox_b, nullptr);
  EXPECT_NE(sandbox_a, sandbox_b);

  // Switching back must return the ORIGINAL sandbox_a pointer — this is
  // FR-018c, "switching back restores all data."
  mgr.SetActiveDomain(a);
  EXPECT_EQ(mgr.GetActiveSandbox(), sandbox_a);
  EXPECT_TRUE(mgr.HasSandbox(b));  // b's sandbox must still exist.
}

TEST(DomainSandboxManagerTest, SetActiveDomainSameOriginIsNoop) {
  DomainSandboxManager mgr(ProfilePath());
  const url::Origin origin = MakeOrigin("https://example.com");

  mgr.SetActiveDomain(origin);
  DomainSandbox* first = mgr.GetActiveSandbox();
  const base::Time first_last_used = first->last_used;

  // Setting the same domain again must not re-touch last_used (contract:
  // the code early-returns without calling GetOrCreateSandbox).
  mgr.SetActiveDomain(origin);
  EXPECT_EQ(mgr.GetActiveSandbox(), first);
  EXPECT_EQ(first->last_used, first_last_used);
}

TEST(DomainSandboxManagerTest, DeleteSandboxRemovesIt) {
  DomainSandboxManager mgr(ProfilePath());
  const url::Origin origin = MakeOrigin("https://example.com");
  mgr.GetOrCreateSandbox(origin);
  EXPECT_TRUE(mgr.HasSandbox(origin));

  mgr.DeleteSandbox(origin);

  EXPECT_FALSE(mgr.HasSandbox(origin));
  EXPECT_TRUE(mgr.GetAllDomains().empty());
}

TEST(DomainSandboxManagerTest, DeletingActiveDomainClearsActiveState) {
  DomainSandboxManager mgr(ProfilePath());
  const url::Origin a = MakeOrigin("https://a.test");
  const url::Origin b = MakeOrigin("https://b.test");
  mgr.SetActiveDomain(a);
  mgr.GetOrCreateSandbox(b);

  mgr.DeleteSandbox(a);

  EXPECT_TRUE(mgr.GetActiveDomain().opaque());
  EXPECT_EQ(mgr.GetActiveSandbox(), nullptr);
  // Deleting active domain must not touch other sandboxes.
  EXPECT_TRUE(mgr.HasSandbox(b));
}

TEST(DomainSandboxManagerTest, DeleteNonActiveSandboxDoesNotAffectActive) {
  DomainSandboxManager mgr(ProfilePath());
  const url::Origin a = MakeOrigin("https://a.test");
  const url::Origin b = MakeOrigin("https://b.test");
  mgr.SetActiveDomain(a);
  mgr.GetOrCreateSandbox(b);

  mgr.DeleteSandbox(b);

  EXPECT_EQ(mgr.GetActiveDomain(), a);
  ASSERT_NE(mgr.GetActiveSandbox(), nullptr);
  EXPECT_EQ(mgr.GetActiveSandbox()->origin, a);
}

TEST(DomainSandboxManagerTest, DeleteNonExistentSandboxIsSilent) {
  DomainSandboxManager mgr(ProfilePath());
  // Must not crash / DCHECK — contract is a DLOG warning and a no-op.
  mgr.DeleteSandbox(MakeOrigin("https://never-created.test"));
}

TEST(DomainSandboxManagerTest, GetAllDomainsReturnsAllCreated) {
  DomainSandboxManager mgr(ProfilePath());
  const url::Origin a = MakeOrigin("https://a.test");
  const url::Origin b = MakeOrigin("https://b.test");
  const url::Origin c = MakeOrigin("https://c.test");
  mgr.GetOrCreateSandbox(a);
  mgr.GetOrCreateSandbox(b);
  mgr.GetOrCreateSandbox(c);

  auto all = mgr.GetAllDomains();
  EXPECT_EQ(all.size(), 3u);
  auto contains = [&all](const url::Origin& o) {
    return std::find(all.begin(), all.end(), o) != all.end();
  };
  EXPECT_TRUE(contains(a));
  EXPECT_TRUE(contains(b));
  EXPECT_TRUE(contains(c));
}

TEST(DomainSandboxManagerTest, ShutdownClearsAllState) {
  DomainSandboxManager mgr(ProfilePath());
  const url::Origin origin = MakeOrigin("https://example.com");
  mgr.SetActiveDomain(origin);
  EXPECT_TRUE(mgr.HasSandbox(origin));

  mgr.Shutdown();

  EXPECT_FALSE(mgr.HasSandbox(origin));
  EXPECT_TRUE(mgr.GetActiveDomain().opaque());
  EXPECT_EQ(mgr.GetActiveSandbox(), nullptr);
  EXPECT_TRUE(mgr.GetAllDomains().empty());
}

}  // namespace
}  // namespace nexus
