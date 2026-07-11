// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/tools/tool_base.h"

#include <string>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

// Subclass that records the OnStop calls it receives. Exercises the
// virtual dispatch path: the framework cascades OnStop(reason) and the
// override is what actually runs cleanup.
class RecordingTool : public ToolBase {
 public:
  RecordingTool(Profile* profile, const std::string& session_key)
      : ToolBase(/*session=*/nullptr, profile, session_key) {}

  void OnStop(OnStopReason reason) override {
    stop_reasons_.push_back(reason);
  }

  const std::vector<OnStopReason>& stop_reasons() const {
    return stop_reasons_;
  }

 private:
  std::vector<OnStopReason> stop_reasons_;
};

TEST(ToolBaseTest, GetSessionKeyReturnsConstructorValue) {
  // Profile* may be null in unit tests — the base class only stores
  // the pointer, never dereferences it. The session-key string is the
  // important contract here.
  ToolBase base(/*session=*/nullptr, /*profile=*/nullptr, "session-A");
  EXPECT_EQ(base.GetSessionKey(), "session-A");
  EXPECT_EQ(base.GetProfile(), nullptr);
}

TEST(ToolBaseTest, EmptySessionKeyIsAcceptedVerbatim) {
  // Empty session keys flow through the framework's `__default__`
  // fallback at the dispatch layer, but ToolBase itself doesn't
  // intervene — it just stores what it's given.
  ToolBase base(/*session=*/nullptr, /*profile=*/nullptr, "");
  EXPECT_EQ(base.GetSessionKey(), "");
}

TEST(ToolBaseTest, OnStopDefaultIsNoOp) {
  ToolBase base(/*session=*/nullptr, /*profile=*/nullptr, "session-noop");
  base.OnStop(OnStopReason::kUserStopped);
  base.OnStop(OnStopReason::kDestructor);
  base.OnStop(OnStopReason::kCompleted);
  SUCCEED();
}

TEST(ToolBaseTest, OnStopOverrideReceivesReason) {
  RecordingTool tool(/*profile=*/nullptr, "session-record");
  tool.OnStop(OnStopReason::kUserStopped);
  tool.OnStop(OnStopReason::kDestructor);

  ASSERT_EQ(tool.stop_reasons().size(), 2u);
  EXPECT_EQ(tool.stop_reasons()[0], OnStopReason::kUserStopped);
  EXPECT_EQ(tool.stop_reasons()[1], OnStopReason::kDestructor);
}

TEST(ToolBaseTest, OnStopIsIdempotentWhenOverrideIsIdempotent) {
  // The framework contract requires every override be idempotent.
  // RecordingTool's override is intentionally non-idempotent (it
  // appends to a vector) to verify nothing in the BASE forces idempotence
  // — the contract is delegated to subclasses.
  RecordingTool tool(/*profile=*/nullptr, "session-x");
  tool.OnStop(OnStopReason::kCompleted);
  tool.OnStop(OnStopReason::kCompleted);
  tool.OnStop(OnStopReason::kCompleted);
  EXPECT_EQ(tool.stop_reasons().size(), 3u);
}

TEST(ToolBaseTest, VirtualDispatchThroughBaseReference) {
  // The framework only ever holds ToolSession::browser_tool_ &c. by
  // value, but the polymorphic OnStop cascade goes through ToolBase&.
  // Confirm the override is reached through the base reference.
  RecordingTool tool(/*profile=*/nullptr, "session-poly");
  ToolBase& base_ref = tool;
  base_ref.OnStop(OnStopReason::kDisconnected);
  base_ref.OnStop(OnStopReason::kShutdown);

  ASSERT_EQ(tool.stop_reasons().size(), 2u);
  EXPECT_EQ(tool.stop_reasons()[0], OnStopReason::kDisconnected);
  EXPECT_EQ(tool.stop_reasons()[1], OnStopReason::kShutdown);
}

TEST(OnStopReasonTest, ParseRecognizedStrings) {
  EXPECT_EQ(ParseOnStopReason("completed"), OnStopReason::kCompleted);
  EXPECT_EQ(ParseOnStopReason("user_stopped"), OnStopReason::kUserStopped);
  EXPECT_EQ(ParseOnStopReason("session_deleted"), OnStopReason::kSessionDeleted);
  EXPECT_EQ(ParseOnStopReason("disconnected"), OnStopReason::kDisconnected);
  EXPECT_EQ(ParseOnStopReason("shutdown"), OnStopReason::kShutdown);
  EXPECT_EQ(ParseOnStopReason("destructor"), OnStopReason::kDestructor);
}

TEST(OnStopReasonTest, ParseUnknownFallsBack) {
  EXPECT_EQ(ParseOnStopReason(""), OnStopReason::kUserStopped);
  EXPECT_EQ(ParseOnStopReason("garbage"), OnStopReason::kUserStopped);
  EXPECT_EQ(ParseOnStopReason("garbage", OnStopReason::kCompleted),
            OnStopReason::kCompleted);
}

TEST(OnStopReasonTest, ToStringMirrorsParse) {
  for (OnStopReason r : {OnStopReason::kCompleted, OnStopReason::kUserStopped,
                          OnStopReason::kSessionDeleted,
                          OnStopReason::kDisconnected, OnStopReason::kShutdown,
                          OnStopReason::kDestructor}) {
    EXPECT_EQ(ParseOnStopReason(OnStopReasonToString(r)), r);
  }
}

}  // namespace
}  // namespace nexus
