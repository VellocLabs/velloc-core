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
      : ToolBase(profile, session_key) {}

  void OnStop(const std::string& reason) override {
    stop_reasons_.push_back(reason);
  }

  const std::vector<std::string>& stop_reasons() const {
    return stop_reasons_;
  }

 private:
  std::vector<std::string> stop_reasons_;
};

TEST(ToolBaseTest, GetSessionKeyReturnsConstructorValue) {
  // Profile* may be null in unit tests — the base class only stores
  // the pointer, never dereferences it. The session-key string is the
  // important contract here.
  ToolBase base(/*profile=*/nullptr, "session-A");
  EXPECT_EQ(base.GetSessionKey(), "session-A");
  EXPECT_EQ(base.GetProfile(), nullptr);
}

TEST(ToolBaseTest, EmptySessionKeyIsAcceptedVerbatim) {
  // Empty session keys flow through the framework's `__default__`
  // fallback at the dispatch layer, but ToolBase itself doesn't
  // intervene — it just stores what it's given.
  ToolBase base(/*profile=*/nullptr, "");
  EXPECT_EQ(base.GetSessionKey(), "");
}

TEST(ToolBaseTest, OnStopDefaultIsNoOp) {
  // The default OnStop body is empty — categories that don't need
  // cleanup don't have to override. This test confirms calling the
  // base directly does nothing observable (no crash, no side effect).
  ToolBase base(/*profile=*/nullptr, "session-noop");
  base.OnStop("user_stopped");
  base.OnStop("destructor");
  base.OnStop("");
  // Successful return is the contract.
  SUCCEED();
}

TEST(ToolBaseTest, OnStopOverrideReceivesReasonString) {
  RecordingTool tool(/*profile=*/nullptr, "session-record");
  tool.OnStop("user_stopped");
  tool.OnStop("destructor");

  ASSERT_EQ(tool.stop_reasons().size(), 2u);
  EXPECT_EQ(tool.stop_reasons()[0], "user_stopped");
  EXPECT_EQ(tool.stop_reasons()[1], "destructor");
}

TEST(ToolBaseTest, OnStopIsIdempotentWhenOverrideIsIdempotent) {
  // The framework contract requires every override be idempotent.
  // RecordingTool's override is intentionally non-idempotent (it
  // appends to a vector) to verify nothing in the BASE forces idempotence
  // — the contract is delegated to subclasses.
  RecordingTool tool(/*profile=*/nullptr, "session-x");
  tool.OnStop("first");
  tool.OnStop("first");
  tool.OnStop("first");
  EXPECT_EQ(tool.stop_reasons().size(), 3u);
}

TEST(ToolBaseTest, VirtualDispatchThroughBaseReference) {
  // The framework only ever holds ToolSession::browser_tool_ &c. by
  // value, but the polymorphic OnStop cascade goes through ToolBase&.
  // Confirm the override is reached through the base reference.
  RecordingTool tool(/*profile=*/nullptr, "session-poly");
  ToolBase& base_ref = tool;
  base_ref.OnStop("disconnected");
  base_ref.OnStop("shutdown");

  ASSERT_EQ(tool.stop_reasons().size(), 2u);
  EXPECT_EQ(tool.stop_reasons()[0], "disconnected");
  EXPECT_EQ(tool.stop_reasons()[1], "shutdown");
}

}  // namespace
}  // namespace nexus
