// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/web_window/sign_in_window.h"

#include <memory>

#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "chrome/test/base/testing_profile.h"
#include "content/public/test/navigation_simulator.h"
#include "content/public/test/web_contents_tester.h"
#include "custom_browser/browser/nexus/api/web_window/sign_in_status_page.h"
#include "custom_browser/browser/nexus/nexus_test_support.h"
#include "net/base/net_errors.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace nexus {
namespace {

class SignInSuccessCloserTest : public ChromeRenderViewHostTestHarness {
 protected:
  SignInSuccessCloserTest()
      : ChromeRenderViewHostTestHarness(
            base::test::TaskEnvironment::TimeSource::MOCK_TIME) {}

  // The harness builds a regular TestingProfile, which would otherwise
  // eagerly construct GlicKeyedService and crash without a real
  // ProfileManager.
  TestingProfile::TestingFactories GetTestingFactories() const override {
    return test::GlicDisablingTestingFactories();
  }

  // Commits `url` and reports its load as finished.
  void FinishLoad(const GURL& url) {
    content::NavigationSimulator::NavigateAndCommitFromBrowser(web_contents(),
                                                               url);
    content::WebContentsTester::For(web_contents())->TestDidFinishLoad(url);
  }

  std::unique_ptr<SignInSuccessCloser> MakeCloser(const GURL& page) {
    return std::make_unique<SignInSuccessCloser>(
        web_contents(), page,
        base::BindOnce([](int* closes) { ++*closes; }, &closes_));
  }

  int closes_ = 0;
};

constexpr char kSuccessPage[] = "data:text/html,success";
constexpr char kFinishingPage[] = "data:text/html,finishing";

// A slow load must not eat into the hold: nothing runs until the page loaded.
TEST_F(SignInSuccessCloserTest, HoldStartsWhenTheSuccessPageLoaded) {
  auto closer = MakeCloser(GURL(kSuccessPage));
  task_environment()->FastForwardBy(kSignInSuccessHold * 3);
  EXPECT_EQ(closes_, 0);

  FinishLoad(GURL(kSuccessPage));
  task_environment()->FastForwardBy(kSignInSuccessHold / 2);
  EXPECT_EQ(closes_, 0);
  task_environment()->FastForwardBy(kSignInSuccessHold);
  EXPECT_EQ(closes_, 1);
}

TEST_F(SignInSuccessCloserTest, AnEarlierStatusPageLoadDoesNotStartTheHold) {
  auto closer = MakeCloser(GURL(kSuccessPage));
  FinishLoad(GURL(kFinishingPage));
  task_environment()->FastForwardBy(kSignInSuccessHold * 2);
  EXPECT_EQ(closes_, 0);
}

TEST_F(SignInSuccessCloserTest, AFailedLoadStillCloses) {
  auto closer = MakeCloser(GURL(kSuccessPage));
  content::NavigationSimulator::NavigateAndCommitFromBrowser(
      web_contents(), GURL(kSuccessPage));
  content::WebContentsTester::For(web_contents())
      ->TestDidFailLoadWithError(GURL(kSuccessPage), net::ERR_ABORTED);
  task_environment()->FastForwardBy(kSignInSuccessHold);
  EXPECT_EQ(closes_, 1);
}

TEST_F(SignInSuccessCloserTest, ClosesOnceAndNeverAfterDestruction) {
  auto closer = MakeCloser(GURL(kSuccessPage));
  FinishLoad(GURL(kSuccessPage));
  content::WebContentsTester::For(web_contents())
      ->TestDidFinishLoad(GURL(kSuccessPage));
  task_environment()->FastForwardBy(kSignInSuccessHold * 2);
  EXPECT_EQ(closes_, 1);

  auto second = MakeCloser(GURL(kSuccessPage));
  FinishLoad(GURL(kSuccessPage));
  second.reset();
  task_environment()->FastForwardBy(kSignInSuccessHold * 2);
  EXPECT_EQ(closes_, 1);
}

}  // namespace
}  // namespace nexus
