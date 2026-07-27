#include "custom_browser/browser/nexus/api/llm/llm_observation.h"

#include <string>

#include "base/memory/scoped_refptr.h"
#include "base/values.h"
#include "net/http/http_response_headers.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

scoped_refptr<net::HttpResponseHeaders> MakeHeaders(const std::string& raw) {
  return net::HttpResponseHeaders::TryToCreate(raw);
}

TEST(LlmObservationTest, CapturesStatusAndAllowlistedHeaders) {
  auto headers = MakeHeaders(
      "HTTP/1.1 200 OK\n"
      "request-id: req_011CQ8xk2Bd9YZ\n"
      "anthropic-ratelimit-requests-remaining: 4999\n"
      "content-type: application/json\n");
  ASSERT_TRUE(headers);

  LlmObservation observation;
  CaptureResponseHeaders(headers.get(), &observation);

  EXPECT_EQ(observation.http_status, 200);
  EXPECT_EQ(observation.headers["request-id"], "req_011CQ8xk2Bd9YZ");
  EXPECT_EQ(observation.headers["anthropic-ratelimit-requests-remaining"],
            "4999");
  // Not on the allowlist -> never copied.
  EXPECT_EQ(observation.headers.count("content-type"), 0u);
}

// The allowlist is the privacy contract: a credential header must never be
// copied even when the provider sends one back.
TEST(LlmObservationTest, NeverCapturesCredentialHeaders) {
  auto headers = MakeHeaders(
      "HTTP/1.1 200 OK\n"
      "authorization: Bearer sk-secret\n"
      "set-cookie: session=abc\n"
      "x-api-key: sk-also-secret\n"
      "request-id: req_ok\n");
  ASSERT_TRUE(headers);

  LlmObservation observation;
  CaptureResponseHeaders(headers.get(), &observation);

  EXPECT_EQ(observation.headers.size(), 1u);
  EXPECT_EQ(observation.headers["request-id"], "req_ok");
  for (const auto& [name, value] : observation.headers) {
    EXPECT_NE(name, "authorization");
    EXPECT_NE(name, "set-cookie");
    EXPECT_NE(name, "x-api-key");
  }
}

TEST(LlmObservationTest, NullHeadersIsANoOp) {
  LlmObservation observation;
  CaptureResponseHeaders(nullptr, &observation);
  EXPECT_FALSE(observation.http_status.has_value());
  EXPECT_TRUE(observation.headers.empty());
}

TEST(LlmObservationTest, CapturesRequestBodySizeAndDigest) {
  LlmObservation observation;
  CaptureRequestBody("abc", &observation);

  EXPECT_EQ(observation.request_body_bytes, 3);
  // Known SHA-256 of "abc", lowercase hex.
  EXPECT_EQ(observation.request_body_digest,
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(LlmObservationTest, CapturesBodyMarkers) {
  base::DictValue body;
  body.Set("id", "msg_01XyZ");
  body.Set("service_tier", "standard");
  body.Set("model", "claude-opus-4-5-20251101");

  LlmObservation observation;
  CaptureResponseBodyMarkers(body, &observation);

  EXPECT_EQ(observation.response_id, "msg_01XyZ");
  EXPECT_EQ(observation.service_tier, "standard");
  EXPECT_EQ(observation.raw_model_id, "claude-opus-4-5-20251101");
}

TEST(LlmObservationTest, MissingBodyMarkersAreSimplyAbsent) {
  base::DictValue body;
  body.Set("content", "irrelevant");

  LlmObservation observation;
  CaptureResponseBodyMarkers(body, &observation);

  EXPECT_FALSE(observation.response_id.has_value());
  EXPECT_FALSE(observation.service_tier.has_value());
  EXPECT_FALSE(observation.raw_model_id.has_value());
}

}  // namespace
}  // namespace nexus
