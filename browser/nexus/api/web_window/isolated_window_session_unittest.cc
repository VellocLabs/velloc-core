// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/web_window/isolated_window_session.h"

#include <memory>
#include <vector>

#include "base/functional/bind.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/global_features.h"
#include "chrome/browser/global_features_test_support.h"
#include "chrome/browser/prefs/incognito_mode_prefs.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/test/fake_global_browser_collection.h"
#include "chrome/browser/ui/browser_window/test/mock_browser_window_interface.h"
#include "chrome/test/base/testing_browser_process.h"
#include "chrome/test/base/testing_profile.h"
#include "components/policy/core/common/policy_pref_names.h"
#include "content/public/test/browser_task_environment.h"
#include "custom_browser/browser/nexus/nexus_test_support.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

class FakeCollectionFeatures : public GlobalFeatures {
 protected:
  std::unique_ptr<GlobalBrowserCollection> CreateGlobalBrowserCollection()
      override {
    return std::make_unique<FakeGlobalBrowserCollection>();
  }
};

std::unique_ptr<GlobalFeatures> CreateFakeCollectionFeatures() {
  return std::make_unique<FakeCollectionFeatures>();
}

using MockBrowser = testing::NiceMock<MockBrowserWindowInterface>;

class IsolatedWindowSessionTest : public testing::Test {
 protected:
  IsolatedWindowSessionTest()
      : features_override_(base::BindRepeating(&CreateFakeCollectionFeatures)) {
  }

  void SetUp() override {
    TestingBrowserProcess::GetGlobal()->SetUpGlobalFeaturesForTesting(
        /*profile_manager=*/false);
    TestingProfile::Builder builder;
    test::AddGlicDisablingFactory(builder);
    profile_ = builder.Build();
  }

  void TearDown() override {
    for (auto& browser : browsers_) {
      if (browser) {
        collection()->SimulateBrowserClosed(browser.get());
      }
    }
    browsers_.clear();
    task_environment_.RunUntilIdle();
    profile_.reset();
    TestingBrowserProcess::GetGlobal()->TearDownGlobalFeaturesForTesting();
  }

  FakeGlobalBrowserCollection* collection() {
    return static_cast<FakeGlobalBrowserCollection*>(
        g_browser_process->GetFeatures()->global_browser_collection());
  }

  // A browser window of `session`, as the collection reports it.
  MockBrowser* OpenWindow(Profile* session) {
    auto browser = std::make_unique<MockBrowser>();
    ON_CALL(*browser, GetProfile()).WillByDefault(testing::Return(session));
    MockBrowser* raw = browser.get();
    collection()->SimulateBrowserCreated(raw);
    browsers_.push_back(std::move(browser));
    return raw;
  }

  void CloseWindow(MockBrowser* browser) {
    collection()->SimulateBrowserClosed(browser);
    for (auto& owned : browsers_) {
      if (owned.get() == browser) {
        owned.reset();
      }
    }
  }

  bool Exists(const Profile::OTRProfileID& id) {
    return profile_->HasOffTheRecordProfile(id);
  }

  content::BrowserTaskEnvironment task_environment_;
  ::test::ScopedGlobalFeaturesOverride features_override_;
  std::unique_ptr<TestingProfile> profile_;
  std::vector<std::unique_ptr<MockBrowser>> browsers_;
};

TEST_F(IsolatedWindowSessionTest, EachCallMakesAFreshOffTheRecordSession) {
  Profile* first = CreateIsolatedWindowSession(profile_.get());
  Profile* second = CreateIsolatedWindowSession(profile_.get());
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_NE(first, second);
  EXPECT_TRUE(first->IsOffTheRecord());
  EXPECT_EQ(first->GetOriginalProfile(), profile_.get());
  EXPECT_FALSE(first->GetOTRProfileID() == Profile::OTRProfileID::PrimaryID());
  EXPECT_TRUE(first->AllowsBrowserWindows());
  DestroyIsolatedWindowSessionWhenUnused(first);
  DestroyIsolatedWindowSessionWhenUnused(second);
  task_environment_.RunUntilIdle();
}

TEST_F(IsolatedWindowSessionTest, NoneWhenPolicyDisablesOffTheRecordWindows) {
  IncognitoModePrefs::SetAvailability(
      profile_->GetPrefs(), policy::IncognitoModeAvailability::kDisabled);
  EXPECT_EQ(CreateIsolatedWindowSession(profile_.get()), nullptr);
}

TEST_F(IsolatedWindowSessionTest, DestroyedSoonWhenNoWindowOpened) {
  Profile* session = CreateIsolatedWindowSession(profile_.get());
  const Profile::OTRProfileID id = session->GetOTRProfileID();
  DestroyIsolatedWindowSessionWhenUnused(session);
  // Never inline: a browser that failed to open may still refer to it.
  EXPECT_TRUE(Exists(id));
  task_environment_.RunUntilIdle();
  EXPECT_FALSE(Exists(id));
}

TEST_F(IsolatedWindowSessionTest, DestroyedAfterItsWindowClosed) {
  Profile* session = CreateIsolatedWindowSession(profile_.get());
  const Profile::OTRProfileID id = session->GetOTRProfileID();
  MockBrowser* window = OpenWindow(session);
  DestroyIsolatedWindowSessionWhenUnused(session);
  task_environment_.RunUntilIdle();
  EXPECT_TRUE(Exists(id));

  CloseWindow(window);
  EXPECT_TRUE(Exists(id));
  task_environment_.RunUntilIdle();
  EXPECT_FALSE(Exists(id));
}

TEST_F(IsolatedWindowSessionTest, KeptWhileAPopupOfTheSessionIsOpen) {
  Profile* session = CreateIsolatedWindowSession(profile_.get());
  const Profile::OTRProfileID id = session->GetOTRProfileID();
  MockBrowser* window = OpenWindow(session);
  DestroyIsolatedWindowSessionWhenUnused(session);
  MockBrowser* popup = OpenWindow(session);
  MockBrowser* unrelated = OpenWindow(profile_.get());

  CloseWindow(window);
  CloseWindow(unrelated);
  task_environment_.RunUntilIdle();
  EXPECT_TRUE(Exists(id));

  CloseWindow(popup);
  task_environment_.RunUntilIdle();
  EXPECT_FALSE(Exists(id));
}

TEST_F(IsolatedWindowSessionTest, StopsWatchingWhenTheSessionGoesFirst) {
  Profile* session = CreateIsolatedWindowSession(profile_.get());
  const Profile::OTRProfileID id = session->GetOTRProfileID();
  MockBrowser* window = OpenWindow(session);
  DestroyIsolatedWindowSessionWhenUnused(session);

  // As at shutdown: the profile is destroyed with its window still listed.
  profile_->DestroyOffTheRecordProfile(session);
  EXPECT_FALSE(Exists(id));
  ON_CALL(*window, GetProfile()).WillByDefault(testing::Return(nullptr));
  CloseWindow(window);
  task_environment_.RunUntilIdle();
}

}  // namespace
}  // namespace nexus
