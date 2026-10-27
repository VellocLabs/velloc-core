// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/web_window/sign_in_status_page.h"

#include <string>

#include "base/strings/escape.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace nexus {
namespace {

SignInStatusText Text() {
  return {u"Finishing sign-in", u"Signed in", u"Closing soon", u"Failed",
          u"Try again"};
}

// The HTML inside a page's data: URL.
std::string Html(const GURL& page) {
  const std::string spec = page.spec();
  const std::string prefix = "data:text/html;charset=utf-8,";
  EXPECT_EQ(0u, spec.rfind(prefix, 0)) << spec;
  return base::UnescapeBinaryURLComponent(spec.substr(prefix.size()));
}

TEST(SignInStatusPageTest, EachStatusShowsItsOwnText) {
  const std::string finishing =
      Html(MakeSignInStatusPage(SignInStatus::kFinishing, Text()));
  EXPECT_NE(std::string::npos, finishing.find("Finishing sign-in"));
  EXPECT_NE(std::string::npos, finishing.find("class=spin"));
  EXPECT_EQ(std::string::npos, finishing.find("Signed in"));

  const std::string succeeded =
      Html(MakeSignInStatusPage(SignInStatus::kSucceeded, Text()));
  EXPECT_NE(std::string::npos, succeeded.find("<h1>Signed in</h1>"));
  EXPECT_NE(std::string::npos, succeeded.find("Closing soon"));
  // The countdown bar runs exactly as long as the window waits.
  EXPECT_NE(std::string::npos, succeeded.find("--hold:1500ms"));

  const std::string failed = Html(
      MakeSignInStatusPage(SignInStatus::kFailed, Text(), u"token refused"));
  EXPECT_NE(std::string::npos, failed.find("<h1>Failed</h1>"));
  EXPECT_NE(std::string::npos, failed.find("token refused"));
  EXPECT_NE(std::string::npos, failed.find("Try again"));
  EXPECT_EQ(std::string::npos, failed.find("class=bar"));
}

TEST(SignInStatusPageTest, ReasonIsEscapedAndThePageRunsNoScript) {
  const std::string html = Html(MakeSignInStatusPage(
      SignInStatus::kFailed, Text(), u"<script>alert(1)</script>&"));
  EXPECT_EQ(std::string::npos, html.find("<script"));
  EXPECT_NE(std::string::npos, html.find("&lt;script&gt;alert(1)"));
  EXPECT_NE(std::string::npos, html.find("default-src 'none'"));
}

TEST(SignInStatusPageTest, ReasonIsCollapsedAndCut) {
  EXPECT_EQ(u"a b c", NormalizeSignInFailureReason(u"  a\n\tb   c "));
  EXPECT_EQ(u"", NormalizeSignInFailureReason(u" \n "));

  const std::u16string exact(kSignInFailureReasonMaxLength, u'x');
  EXPECT_EQ(exact, NormalizeSignInFailureReason(exact));

  const std::u16string cut =
      NormalizeSignInFailureReason(exact + u"overflow");
  EXPECT_EQ(kSignInFailureReasonMaxLength, cut.size());
  EXPECT_EQ(u'…', cut.back());

  // A surrogate pair straddling the cut is dropped whole.
  std::u16string pair(kSignInFailureReasonMaxLength - 2, u'y');
  pair += u"\U0001F600tail";
  const std::u16string no_half = NormalizeSignInFailureReason(pair);
  EXPECT_EQ(std::u16string(kSignInFailureReasonMaxLength - 2, u'y') + u"…",
            no_half);
}

TEST(SignInStatusPageTest, AnEmptyReasonShowsNoReasonLine) {
  const std::string html =
      Html(MakeSignInStatusPage(SignInStatus::kFailed, Text(), u"  "));
  EXPECT_EQ(std::string::npos, html.find("class=reason"));
}

TEST(SignInStatusPageTest, OAuthErrorCodesMapToAKindNotToText) {
  EXPECT_EQ(SignInFailureKind::kDenied,
            ClassifyOAuthSignInError("access_denied"));
  EXPECT_EQ(SignInFailureKind::kExpired,
            ClassifyOAuthSignInError("expired_token"));
  EXPECT_EQ(SignInFailureKind::kServiceError,
            ClassifyOAuthSignInError("server_error"));
  EXPECT_EQ(SignInFailureKind::kServiceError,
            ClassifyOAuthSignInError("invalid_request"));
  EXPECT_EQ(SignInFailureKind::kServiceError,
            ClassifyOAuthSignInError("Access_Denied"));
}

}  // namespace
}  // namespace nexus
