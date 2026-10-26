// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/web_window/web_window_options.h"

#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace nexus {
namespace {

TEST(WebWindowOptionsTest, OnlyWebAndFileUrlsAreAllowed) {
  EXPECT_TRUE(IsAllowedWebWindowUrl(GURL("https://example.com/a")));
  EXPECT_TRUE(IsAllowedWebWindowUrl(GURL("http://127.0.0.1:8080/")));
  EXPECT_TRUE(IsAllowedWebWindowUrl(GURL("file:///C:/tmp/a.html")));

  EXPECT_FALSE(IsAllowedWebWindowUrl(GURL("chrome://settings")));
  EXPECT_FALSE(IsAllowedWebWindowUrl(GURL("devtools://devtools/x.html")));
  EXPECT_FALSE(IsAllowedWebWindowUrl(GURL("javascript:alert(1)")));
  EXPECT_FALSE(IsAllowedWebWindowUrl(GURL("data:text/html,hi")));
  EXPECT_FALSE(IsAllowedWebWindowUrl(GURL("about:blank")));
  EXPECT_FALSE(IsAllowedWebWindowUrl(GURL("not a url")));
  EXPECT_FALSE(IsAllowedWebWindowUrl(GURL()));
}

TEST(WebWindowOptionsTest, StateNamesRoundTrip) {
  EXPECT_EQ(ui::mojom::WindowShowState::kNormal,
            ParseWebWindowState("normal"));
  EXPECT_EQ(ui::mojom::WindowShowState::kMaximized,
            ParseWebWindowState("maximized"));
  EXPECT_EQ(ui::mojom::WindowShowState::kMinimized,
            ParseWebWindowState("minimized"));
  EXPECT_FALSE(ParseWebWindowState("fullscreen").has_value());
  EXPECT_FALSE(ParseWebWindowState("").has_value());

  EXPECT_EQ("normal", WebWindowStateName(false, false));
  EXPECT_EQ("maximized", WebWindowStateName(true, false));
  EXPECT_EQ("minimized", WebWindowStateName(false, true));
  EXPECT_EQ("minimized", WebWindowStateName(true, true));
}

TEST(WebWindowOptionsTest, AbsentBoundsFieldsKeepTheCurrentValue) {
  const gfx::Rect current(10, 20, 800, 600);
  gfx::Rect out;

  EXPECT_FALSE(ResolveWebWindowBounds({}, current, out).has_value());
  EXPECT_EQ(current, out);

  WebWindowBoundsRequest move;
  move.left = 100;
  EXPECT_FALSE(ResolveWebWindowBounds(move, current, out).has_value());
  EXPECT_EQ(gfx::Rect(100, 20, 800, 600), out);

  WebWindowBoundsRequest resize;
  resize.width = 400;
  resize.height = 300;
  EXPECT_FALSE(ResolveWebWindowBounds(resize, current, out).has_value());
  EXPECT_EQ(gfx::Rect(10, 20, 400, 300), out);
}

TEST(WebWindowOptionsTest, SizesBelowTheMinimumAreRefused) {
  const gfx::Rect current(0, 0, 800, 600);
  gfx::Rect out = current;

  WebWindowBoundsRequest narrow;
  narrow.width = kWebWindowMinWidth - 1;
  EXPECT_TRUE(ResolveWebWindowBounds(narrow, current, out).has_value());

  WebWindowBoundsRequest short_window;
  short_window.height = kWebWindowMinHeight - 1;
  EXPECT_TRUE(ResolveWebWindowBounds(short_window, current, out).has_value());

  EXPECT_EQ(current, out);
}

TEST(WebWindowOptionsTest, CaptureTargetsAreLoopbackHttpWithAPort) {
  EXPECT_TRUE(IsValidWebWindowCaptureTarget(
      GURL("http://localhost:33418/callback")));
  EXPECT_TRUE(IsValidWebWindowCaptureTarget(GURL("http://127.0.0.1:8080/")));
  EXPECT_TRUE(IsValidWebWindowCaptureTarget(GURL("http://[::1]:9000/cb")));
  EXPECT_TRUE(IsValidWebWindowCaptureTarget(GURL("http://LOCALHOST:5000/")));

  EXPECT_FALSE(IsValidWebWindowCaptureTarget(GURL("https://localhost:443/")));
  EXPECT_FALSE(IsValidWebWindowCaptureTarget(GURL("http://localhost/cb")));
  EXPECT_FALSE(
      IsValidWebWindowCaptureTarget(GURL("http://example.com:8080/cb")));
  EXPECT_FALSE(IsValidWebWindowCaptureTarget(GURL("http://127.0.0.2:8080/")));
  EXPECT_FALSE(
      IsValidWebWindowCaptureTarget(GURL("http://user:pw@localhost:8080/")));
  EXPECT_FALSE(IsValidWebWindowCaptureTarget(GURL("http://u@localhost:8080/")));
  EXPECT_FALSE(IsValidWebWindowCaptureTarget(GURL("file:///C:/cb")));
  EXPECT_FALSE(IsValidWebWindowCaptureTarget(GURL("not a url")));
}

TEST(WebWindowOptionsTest, CaptureMatchIgnoresQueryAndFragmentOnly) {
  const GURL target("http://localhost:33418/callback");

  EXPECT_TRUE(MatchesWebWindowCaptureTarget(target, target));
  EXPECT_TRUE(MatchesWebWindowCaptureTarget(
      target, GURL("http://localhost:33418/callback?code=abc&state=x")));
  EXPECT_TRUE(MatchesWebWindowCaptureTarget(
      target, GURL("http://LocalHost:33418/callback#frag")));

  EXPECT_FALSE(MatchesWebWindowCaptureTarget(
      target, GURL("http://localhost:33419/callback?code=abc")));
  EXPECT_FALSE(MatchesWebWindowCaptureTarget(
      target, GURL("http://localhost:33418/callback/extra")));
  EXPECT_FALSE(MatchesWebWindowCaptureTarget(
      target, GURL("http://127.0.0.1:33418/callback")));
  EXPECT_FALSE(MatchesWebWindowCaptureTarget(
      target, GURL("https://localhost:33418/callback")));
  EXPECT_FALSE(MatchesWebWindowCaptureTarget(target, GURL()));
}

}  // namespace
}  // namespace nexus
