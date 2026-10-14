// Copyright 2026 VellocLabs
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/network/nexus_guest_request.h"

#include "net/base/url_util.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace nexus {
namespace {

TEST(NexusGuestRequestTest, HeadersSetHeaderWouldCrashOnAreRefused) {
  EXPECT_FALSE(InvalidGuestHeader("X-Api-Key", "abc").has_value());
  EXPECT_FALSE(InvalidGuestHeader("X-Empty", "").has_value());
  // A space in the name, CR/LF/NUL in the value: each would CHECK in
  // net::HttpRequestHeaders::SetHeader.
  EXPECT_TRUE(InvalidGuestHeader("X Api Key", "abc").has_value());
  EXPECT_TRUE(InvalidGuestHeader("", "abc").has_value());
  EXPECT_TRUE(InvalidGuestHeader("X-Note", "a\nb").has_value());
  EXPECT_TRUE(InvalidGuestHeader("X-Note", "a\r\nX-Evil: 1").has_value());
  EXPECT_TRUE(
      InvalidGuestHeader("X-Note", std::string_view("a\0b", 3)).has_value());
}

TEST(NexusGuestRequestTest, NothingExpandedLeavesTheUrlAlone) {
  GuestVisibleUrl visible;
  const GURL url("https://api.example.com/x?k=v");
  visible.SetUrls(url, url);
  EXPECT_EQ(visible.For(url), url.spec());
  EXPECT_EQ(visible.For(GURL("https://other.example/y")),
            "https://other.example/y");
}

TEST(NexusGuestRequestTest, TheSentUrlShowsThePlaceholder) {
  const GURL base("https://api.example.com/x");
  const GURL sent = net::AppendQueryParameter(base, "k", "tok 123");
  const GURL guest =
      net::AppendQueryParameter(base, "k", "{{secret:provider:acme}}");
  GuestVisibleUrl visible;
  visible.SetUrls(sent, guest);
  visible.AddExpansion("{{secret:provider:acme}}", "tok 123");
  const std::string shown = visible.For(sent);
  EXPECT_EQ(shown, guest.spec());
  EXPECT_EQ(shown.find("tok"), std::string::npos);
}

TEST(NexusGuestRequestTest, ARedirectEchoingTheSecretIsRedacted) {
  const GURL base("https://api.example.com/x");
  GuestVisibleUrl visible;
  visible.SetUrls(net::AppendQueryParameter(base, "k", "tok 123"),
                  net::AppendQueryParameter(base, "k", "{{oauth:access_token}}"));
  visible.AddExpansion("{{oauth:access_token}}", "tok 123");
  // The server re-encoded the space as %20 and moved the value elsewhere.
  const std::string shown =
      visible.For(GURL("https://api.example.com/y?token=tok%20123&k=tok+123"));
  EXPECT_EQ(shown.find("123"), std::string::npos) << shown;
  EXPECT_NE(shown.find("/y?token="), std::string::npos) << shown;
}

}  // namespace
}  // namespace nexus
