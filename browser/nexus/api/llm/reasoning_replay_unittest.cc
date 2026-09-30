// Copyright 2026 The Custom Browser Authors. All rights reserved.

#include "custom_browser/browser/nexus/api/llm/reasoning_replay.h"

#include <optional>
#include <string>

#include "base/values.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

base::DictValue DetailsReplayPart() {
  base::DictValue detail;
  detail.Set("type", "reasoning.text");
  detail.Set("text", "think");
  detail.Set("signature", "sig-abc");
  base::ListValue details;
  details.Append(std::move(detail));

  base::DictValue part;
  part.Set("type", kReasoningReplayPartType);
  part.Set("field", "reasoning_details");
  part.Set("value", std::move(details));
  return part;
}

TEST(ReasoningReplayTest, RoundTripsForTheSameProviderAndModel) {
  std::optional<std::string> signature = EncodeReasoningReplaySignature(
      "custom-openai-1", "anthropic/claude-sonnet-5.5", DetailsReplayPart());
  ASSERT_TRUE(signature.has_value());
  EXPECT_TRUE(signature->starts_with("velloc-replay-v1:"));

  std::optional<base::DictValue> part = DecodeReasoningReplaySignatureFor(
      *signature, "custom-openai-1", "anthropic/claude-sonnet-5.5");
  ASSERT_TRUE(part.has_value());
  EXPECT_EQ(*part, DetailsReplayPart());
}

TEST(ReasoningReplayTest, OtherProviderOrModelGetsNothing) {
  const std::string signature = *EncodeReasoningReplaySignature(
      "custom-openai-1", "anthropic/claude-sonnet-5.5", DetailsReplayPart());
  EXPECT_FALSE(DecodeReasoningReplaySignatureFor(
      signature, "kimi", "anthropic/claude-sonnet-5.5"));
  EXPECT_FALSE(DecodeReasoningReplaySignatureFor(
      signature, "custom-openai-1", "google/gemini-3-pro"));
}

TEST(ReasoningReplayTest, ForeignOrBrokenSignaturesGetNothing) {
  // An Anthropic thinking signature is not ours.
  EXPECT_FALSE(DecodeReasoningReplaySignatureFor("EqQBCkgIARABGAIi", "p", "m"));
  EXPECT_FALSE(
      DecodeReasoningReplaySignatureFor("velloc-replay-v1:{not json", "p", "m"));
  EXPECT_FALSE(DecodeReasoningReplaySignatureFor(
      R"(velloc-replay-v1:{"provider":"p","model":"m","field":"f"})", "p",
      "m"));
}

TEST(ReasoningReplayTest, MalformedPartsAreNotEncoded) {
  base::DictValue no_field;
  no_field.Set("value", "x");
  EXPECT_FALSE(EncodeReasoningReplaySignature("p", "m", no_field));

  base::DictValue no_value;
  no_value.Set("field", "reasoning_details");
  EXPECT_FALSE(EncodeReasoningReplaySignature("p", "m", no_value));

  EXPECT_FALSE(EncodeReasoningReplaySignature("", "m", DetailsReplayPart()));
  EXPECT_FALSE(EncodeReasoningReplaySignature("p", "", DetailsReplayPart()));
}

}  // namespace
}  // namespace nexus
