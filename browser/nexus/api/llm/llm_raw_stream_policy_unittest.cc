// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/llm/llm_raw_stream_policy.h"

#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace nexus {
namespace {

TEST(LlmRawStreamPolicyTest, AllowsSameHostAndSiblingSubdomain) {
  const GURL base("https://api2.example.com/v1");
  EXPECT_FALSE(CheckRawStreamTarget(base, GURL("https://api2.example.com/x")));
  // A provider whose inference host differs from its auth host.
  EXPECT_FALSE(CheckRawStreamTarget(
      base, GURL("https://agent.us.api5.example.com/svc.Service/Run")));
}

TEST(LlmRawStreamPolicyTest, RejectsForeignDomain) {
  const GURL base("https://api2.example.com/v1");
  EXPECT_TRUE(CheckRawStreamTarget(base, GURL("https://example.net/steal")));
  EXPECT_TRUE(
      CheckRawStreamTarget(base, GURL("https://example.com.evil.org/x")));
}

TEST(LlmRawStreamPolicyTest, RejectsSchemeDowngradeAndCredentials) {
  const GURL base("https://api.example.com");
  EXPECT_TRUE(CheckRawStreamTarget(base, GURL("http://api.example.com/x")));
  EXPECT_TRUE(
      CheckRawStreamTarget(base, GURL("https://u:p@api.example.com/x")));
  EXPECT_TRUE(CheckRawStreamTarget(base, GURL("ftp://api.example.com/x")));
  EXPECT_TRUE(CheckRawStreamTarget(base, GURL("not a url")));
}

TEST(LlmRawStreamPolicyTest, LocalHttpProviderStaysOnItsHost) {
  const GURL base("http://127.0.0.1:11434");
  EXPECT_FALSE(CheckRawStreamTarget(base, GURL("http://127.0.0.1:11434/api")));
  EXPECT_TRUE(CheckRawStreamTarget(base, GURL("http://192.168.1.2/api")));
}

TEST(LlmRawStreamPolicyTest, RejectsMissingBaseUrl) {
  EXPECT_TRUE(CheckRawStreamTarget(GURL(), GURL("https://api.example.com")));
}

TEST(LlmRawStreamPolicyTest, ForbiddenHeaders) {
  EXPECT_TRUE(IsForbiddenRawStreamHeader("Cookie"));
  EXPECT_TRUE(IsForbiddenRawStreamHeader("HOST"));
  EXPECT_TRUE(IsForbiddenRawStreamHeader("content-length"));
  EXPECT_TRUE(IsForbiddenRawStreamHeader("Proxy-Authorization"));
  EXPECT_TRUE(IsForbiddenRawStreamHeader(""));
  // Auth, User-Agent and protocol headers are the guest's to set.
  EXPECT_FALSE(IsForbiddenRawStreamHeader("Authorization"));
  EXPECT_FALSE(IsForbiddenRawStreamHeader("x-api-key"));
  EXPECT_FALSE(IsForbiddenRawStreamHeader("User-Agent"));
  EXPECT_FALSE(IsForbiddenRawStreamHeader("anthropic-version"));
  EXPECT_FALSE(IsForbiddenRawStreamHeader("connect-protocol-version"));
}

SecretLookup FakeSecrets() {
  return base::BindRepeating(
      [](const std::string& name) -> SecretLookupOutcome {
        if (name == "api_key") {
          return {"sk-123"};
        }
        if (name == "access_token") {
          return {"at-456"};
        }
        if (name == "empty") {
          return {""};
        }
        if (name == "sealed_elsewhere") {
          return {std::nullopt, SecretProblem::kUnreadable};
        }
        if (name == "not_yet") {
          return {std::nullopt, SecretProblem::kKeystoreNotReady};
        }
        return {};
      });
}

TEST(LlmRawStreamPolicyTest, ExpandsSecretTemplatesForEveryAuthScheme) {
  std::string out;
  EXPECT_FALSE(ExpandSecretTemplates("Bearer {{secret:api_key}}",
                                     FakeSecrets(), &out));
  EXPECT_EQ(out, "Bearer sk-123");
  EXPECT_FALSE(ExpandSecretTemplates("{{secret:api_key}}", FakeSecrets(), &out));
  EXPECT_EQ(out, "sk-123");
  EXPECT_FALSE(ExpandSecretTemplates(
      "{{secret:api_key}}:{{secret:access_token}}", FakeSecrets(), &out));
  EXPECT_EQ(out, "sk-123:at-456");
  EXPECT_FALSE(ExpandSecretTemplates("2023-06-01", FakeSecrets(), &out));
  EXPECT_EQ(out, "2023-06-01");
  // A lone brace pair that is not a secret placeholder is plain text.
  EXPECT_FALSE(ExpandSecretTemplates("{{other}}", FakeSecrets(), &out));
  EXPECT_EQ(out, "{{other}}");
}

TEST(LlmRawStreamPolicyTest, RefusesUnknownMalformedOrEmptySecrets) {
  std::string out;
  EXPECT_TRUE(ExpandSecretTemplates("Bearer {{secret:nope}}", FakeSecrets(),
                                    &out));
  EXPECT_TRUE(ExpandSecretTemplates("{{secret:empty}}", FakeSecrets(), &out));
  EXPECT_TRUE(ExpandSecretTemplates("{{secret:api_key", FakeSecrets(), &out));
  EXPECT_TRUE(ExpandSecretTemplates("{{secret:a b}}", FakeSecrets(), &out));
  EXPECT_TRUE(ExpandSecretTemplates("{{secret:}}", FakeSecrets(), &out));
  EXPECT_FALSE(IsValidSecretName(std::string(65, 'a')));
  EXPECT_TRUE(IsValidSecretName("refresh_token.v2-x"));
}

// The refusal names the secret and why it has no value; a malformed
// placeholder names neither.
TEST(LlmRawStreamPolicyTest, SecretRefusalCarriesNameAndProblem) {
  std::string out;
  struct Case {
    const char* text;
    SecretProblem problem;
    const char* wire;
  };
  for (const Case& c : {
           Case{"Bearer {{secret:nope}}", SecretProblem::kMissing, "missing"},
           Case{"{{secret:empty}}", SecretProblem::kMissing, "missing"},
           Case{"Bearer {{secret:sealed_elsewhere}}",
                SecretProblem::kUnreadable, "unreadable"},
           Case{"{{secret:not_yet}}", SecretProblem::kKeystoreNotReady,
                "keystore_not_ready"},
       }) {
    const std::optional<SecretTemplateError> error =
        ExpandSecretTemplates(c.text, FakeSecrets(), &out);
    ASSERT_TRUE(error) << c.text;
    ASSERT_TRUE(error->problem) << c.text;
    EXPECT_EQ(*error->problem, c.problem) << c.text;
    EXPECT_STREQ(SecretProblemWireName(*error->problem), c.wire);
    EXPECT_FALSE(error->secret.empty()) << c.text;
  }
  const std::optional<SecretTemplateError> unreadable = ExpandSecretTemplates(
      "{{secret:sealed_elsewhere}}", FakeSecrets(), &out);
  EXPECT_EQ(unreadable->secret, "sealed_elsewhere");
  EXPECT_NE(unreadable->message.find("cannot be decrypted"), std::string::npos);

  const std::optional<SecretTemplateError> malformed =
      ExpandSecretTemplates("{{secret:api_key", FakeSecrets(), &out);
  ASSERT_TRUE(malformed);
  EXPECT_FALSE(malformed->problem);
  EXPECT_TRUE(malformed->secret.empty());
}

TEST(LlmRawStreamPolicyTest, CapturesAndRedactsJsonSecrets) {
  std::map<std::string, std::string> captured;
  std::optional<std::string> body = ApplySecretCaptures(
      R"({"accessToken":"at-1","nested":{"refresh":"rt-2"},"list":["x"],)"
      R"("expiresIn":3600,"a/b":"slash"})",
      {{"/accessToken", "access_token"},
       {"/nested/refresh", "refresh_token"},
       {"/expiresIn", "not_a_string"},
       {"/missing", "missing"},
       {"/a~1b", "escaped"},
       {"/list/0", "listed"}},
      &captured);
  ASSERT_TRUE(body);
  EXPECT_EQ(captured.size(), 4u);
  EXPECT_EQ(captured["access_token"], "at-1");
  EXPECT_EQ(captured["refresh_token"], "rt-2");
  EXPECT_EQ(captured["escaped"], "slash");
  EXPECT_EQ(captured["listed"], "x");
  EXPECT_EQ(body->find("at-1"), std::string::npos);
  EXPECT_EQ(body->find("rt-2"), std::string::npos);
  EXPECT_NE(body->find("[captured]"), std::string::npos);
  EXPECT_NE(body->find("3600"), std::string::npos);
}

TEST(LlmRawStreamPolicyTest, CaptureRefusesNonJsonBodies) {
  std::map<std::string, std::string> captured;
  EXPECT_FALSE(ApplySecretCaptures("token=at-1", {{"/token", "t"}}, &captured));
  EXPECT_TRUE(captured.empty());
}

TEST(LlmRawStreamPolicyTest, ChunkQueueTakesAcrossChunkBoundaries) {
  RawStreamChunkQueue queue;
  EXPECT_TRUE(queue.empty());
  queue.Append("abc");
  queue.Append("");
  queue.Append("defgh");
  EXPECT_EQ(queue.size(), 8u);
  EXPECT_EQ(queue.Take(2), "ab");
  EXPECT_EQ(queue.Take(4), "cdef");
  EXPECT_EQ(queue.size(), 2u);
  EXPECT_EQ(queue.Take(100), "gh");
  EXPECT_TRUE(queue.empty());
  EXPECT_EQ(queue.Take(10), "");
}

TEST(LlmRawStreamPolicyTest, ChunkQueuePreservesBinaryBytes) {
  RawStreamChunkQueue queue;
  const std::string framed("\x00\x00\x00\x00\x02\x08\x01", 7);
  queue.Append(framed);
  EXPECT_EQ(queue.Take(7), framed);
}

TEST(LlmRawStreamPolicyTest, ClientStreamIdBoundsAndCharset) {
  EXPECT_TRUE(IsValidClientStreamId("0123abcd"));
  EXPECT_TRUE(IsValidClientStreamId("3f2b9c1e-6a4d-4e8b-9f00-1a2b3c4d5e6f"));
  EXPECT_TRUE(IsValidClientStreamId("raw_Stream-ID_9"));
  EXPECT_TRUE(IsValidClientStreamId(std::string(64, 'a')));
  EXPECT_FALSE(IsValidClientStreamId(""));
  EXPECT_FALSE(IsValidClientStreamId("1234567"));
  EXPECT_FALSE(IsValidClientStreamId(std::string(65, 'a')));
  EXPECT_FALSE(IsValidClientStreamId("abcd efgh"));
  EXPECT_FALSE(IsValidClientStreamId("abcd/efgh"));
  EXPECT_FALSE(IsValidClientStreamId("abcd.efgh"));
  EXPECT_FALSE(IsValidClientStreamId("abcdefgÃ©"));
}

TEST(LlmRawStreamPolicyTest, RecentIdSetTakesOnce) {
  RecentIdSet set;
  EXPECT_FALSE(set.Take("stream-a"));
  set.Insert("stream-a");
  set.Insert("stream-a");
  EXPECT_EQ(set.size(), 1u);
  EXPECT_TRUE(set.Take("stream-a"));
  EXPECT_FALSE(set.Take("stream-a"));
  EXPECT_EQ(set.size(), 0u);
}

TEST(LlmRawStreamPolicyTest, RecentIdSetEvictsOldestPastCapacity) {
  RecentIdSet set;
  for (size_t i = 0; i <= RecentIdSet::kCapacity; ++i) {
    set.Insert("id-" + base::NumberToString(i));
  }
  EXPECT_EQ(set.size(), RecentIdSet::kCapacity);
  // The first one inserted is gone; the rest are all still there.
  EXPECT_FALSE(set.Take("id-0"));
  EXPECT_TRUE(set.Take("id-1"));
  EXPECT_TRUE(set.Take("id-" + base::NumberToString(RecentIdSet::kCapacity)));
}

TEST(LlmRawStreamPolicyTest, OAuthTemplatesExpandOnlyWithValues) {
  const OAuthValues values{"at-123", "acct-9"};
  std::string out;
  EXPECT_FALSE(ExpandOAuthTemplates("Bearer {{oauth:access_token}}", &values,
                                    &out));
  EXPECT_EQ(out, "Bearer at-123");
  EXPECT_FALSE(ExpandOAuthTemplates("{{oauth:account_id}}", &values, &out));
  EXPECT_EQ(out, "acct-9");
  // Secret placeholders pass through for the secret pass that follows.
  EXPECT_FALSE(ExpandOAuthTemplates("{{secret:api_key}}", nullptr, &out));
  EXPECT_EQ(out, "{{secret:api_key}}");

  // No values (not an OAuth provider / refresh failed), an empty token, an
  // unknown name, an unterminated placeholder: all refuse.
  EXPECT_TRUE(
      ExpandOAuthTemplates("{{oauth:access_token}}", nullptr, &out).has_value());
  const OAuthValues no_token{"", "acct"};
  EXPECT_TRUE(ExpandOAuthTemplates("{{oauth:access_token}}", &no_token, &out)
                  .has_value());
  EXPECT_TRUE(
      ExpandOAuthTemplates("{{oauth:account_id}}", nullptr, &out).has_value());
  EXPECT_TRUE(
      ExpandOAuthTemplates("{{oauth:refresh_token}}", &values, &out).has_value());
  EXPECT_TRUE(
      ExpandOAuthTemplates("{{oauth:access_token", &values, &out).has_value());
  EXPECT_TRUE(HasOAuthTemplate("x {{oauth:access_token}}"));
  EXPECT_FALSE(HasOAuthTemplate("{{secret:api_key}}"));
}

// A ChatGPT sign-in may carry no account id: the placeholder expands to ""
// and the caller omits the header (the old C++ chatgpt-pro behaviour).
TEST(LlmRawStreamPolicyTest, AnEmptyAccountIdOmitsItsValue) {
  const OAuthValues no_account{"at", ""};
  std::string out;
  EXPECT_FALSE(
      ExpandOAuthTemplates("{{oauth:account_id}}", &no_account, &out));
  EXPECT_EQ(out, "");
  EXPECT_TRUE(IsOmittedOAuthValue("{{oauth:account_id}}", out));

  // Only a value that came out empty because of a placeholder is omitted.
  EXPECT_FALSE(ExpandOAuthTemplates("acct-{{oauth:account_id}}", &no_account,
                                    &out));
  EXPECT_EQ(out, "acct-");
  EXPECT_FALSE(IsOmittedOAuthValue("acct-{{oauth:account_id}}", out));
  EXPECT_FALSE(IsOmittedOAuthValue("", ""));
  const OAuthValues values{"at", "acct-9"};
  EXPECT_FALSE(ExpandOAuthTemplates("{{oauth:account_id}}", &values, &out));
  EXPECT_FALSE(IsOmittedOAuthValue("{{oauth:account_id}}", out));
}

}  // namespace
}  // namespace nexus
