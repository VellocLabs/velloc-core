// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/web_window/web_window_storage.h"

#include <memory>
#include <string>

#include "base/run_loop.h"
#include "base/test/test_future.h"
#include "chrome/test/base/testing_profile.h"
#include "content/public/browser/site_instance.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_task_environment.h"
#include "content/public/test/test_renderer_host.h"
#include "custom_browser/browser/nexus/nexus_test_support.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace nexus {
namespace {

constexpr char kName[] = "vellocprofileb";

class WebWindowStorageTest : public testing::Test {
 protected:
  void SetUp() override {
    TestingProfile::Builder builder;
    test::AddGlicDisablingFactory(builder);
    profile_ = builder.Build();
  }

  content::BrowserTaskEnvironment task_environment_;
  content::RenderViewHostTestEnabler rvh_enabler_;
  std::unique_ptr<TestingProfile> profile_;
};

TEST_F(WebWindowStorageTest, NameIsEightToThirtyTwoLowerCaseLetters) {
  EXPECT_TRUE(IsWebWindowStorageName("abcdefgh"));
  EXPECT_TRUE(IsWebWindowStorageName(std::string(32, 'z')));
  EXPECT_FALSE(IsWebWindowStorageName("abcdefg"));
  EXPECT_FALSE(IsWebWindowStorageName(std::string(33, 'z')));
  EXPECT_FALSE(IsWebWindowStorageName("abcdefgH"));
  EXPECT_FALSE(IsWebWindowStorageName("abcdefg1"));
  EXPECT_FALSE(IsWebWindowStorageName("abcd/efgh"));
  EXPECT_FALSE(IsWebWindowStorageName(""));
}

TEST_F(WebWindowStorageTest, EachFreshSessionIsItsOwnAndInMemory) {
  auto first = WebWindowStorage::Fresh(profile_.get());
  auto second = WebWindowStorage::Fresh(profile_.get());
  EXPECT_NE(first->config(), second->config());
  EXPECT_TRUE(first->config().in_memory());
  EXPECT_FALSE(first->config().is_default());
  EXPECT_NE(profile_->GetStoragePartition(first->config()),
            profile_->GetDefaultStoragePartition());
  EXPECT_NE(profile_->GetStoragePartition(first->config()),
            profile_->GetStoragePartition(second->config()));
}

TEST_F(WebWindowStorageTest, AKeptSessionIsTheSameEachTimeAndOnDisk) {
  auto first = WebWindowStorage::Kept(profile_.get(), kName);
  auto again = WebWindowStorage::Kept(profile_.get(), kName);
  auto other = WebWindowStorage::Kept(profile_.get(), "vellocprofilec");
  EXPECT_EQ(first->config(), again->config());
  EXPECT_NE(first->config(), other->config());
  EXPECT_FALSE(first->config().in_memory());
  EXPECT_FALSE(first->config().is_default());
  // A kept name never names a fresh session's domain.
  auto fresh = WebWindowStorage::Fresh(profile_.get());
  auto named_like = WebWindowStorage::Kept(profile_.get(), "vellocfresh");
  EXPECT_NE(fresh->config().partition_domain(),
            named_like->config().partition_domain());
}

TEST_F(WebWindowStorageTest, InUseWhileHeld) {
  EXPECT_FALSE(IsWebWindowStorageInUse(profile_.get(), kName));
  auto first = WebWindowStorage::Kept(profile_.get(), kName);
  auto second = WebWindowStorage::Kept(profile_.get(), kName);
  EXPECT_TRUE(IsWebWindowStorageInUse(profile_.get(), kName));
  EXPECT_FALSE(IsWebWindowStorageInUse(profile_.get(), "vellocprofilec"));
  first.reset();
  EXPECT_TRUE(IsWebWindowStorageInUse(profile_.get(), kName));
  second.reset();
  EXPECT_FALSE(IsWebWindowStorageInUse(profile_.get(), kName));
}

TEST_F(WebWindowStorageTest, ATabMadeInASessionUsesItsPartition) {
  auto storage = WebWindowStorage::Kept(profile_.get(), kName);
  const GURL url("https://accounts.vendor-a.test/authorize");
  content::WebContents::CreateParams params(
      profile_.get(), content::SiteInstance::CreateForFixedStoragePartition(
                          profile_.get(), url, storage->config()));
  std::unique_ptr<content::WebContents> contents =
      content::WebContents::Create(params);
  EXPECT_EQ(contents->GetPrimaryMainFrame()->GetStoragePartition(),
            profile_->GetStoragePartition(storage->config()));
  EXPECT_NE(contents->GetPrimaryMainFrame()->GetStoragePartition(),
            profile_->GetDefaultStoragePartition());
}

TEST_F(WebWindowStorageTest, DeleteAnswersForALoadedAndAnUnloadedSession) {
  {
    auto storage = WebWindowStorage::Kept(profile_.get(), kName);
    profile_->GetStoragePartition(storage->config());
  }
  base::test::TestFuture<void> loaded;
  DeleteWebWindowStorage(profile_.get(), kName, loaded.GetCallback());
  EXPECT_TRUE(loaded.Wait());

  base::test::TestFuture<void> never_opened;
  DeleteWebWindowStorage(profile_.get(), "vellocprofilec",
                         never_opened.GetCallback());
  EXPECT_TRUE(never_opened.Wait());
}

TEST_F(WebWindowStorageTest, LettingGoAfterTheProfileIsGoneIsSafe) {
  auto fresh = WebWindowStorage::Fresh(profile_.get());
  auto kept = WebWindowStorage::Kept(profile_.get(), kName);
  profile_->GetStoragePartition(fresh->config());
  profile_.reset();
  fresh.reset();
  kept.reset();
}

}  // namespace
}  // namespace nexus
