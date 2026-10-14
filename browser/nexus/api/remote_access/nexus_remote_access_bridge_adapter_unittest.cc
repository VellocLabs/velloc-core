// Copyright 2026 VellocLabs
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/remote_access/nexus_remote_access_bridge_adapter.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/values.h"
#include "content/public/test/browser_task_environment.h"
#include "custom_browser/browser/nexus/nexus_bridge_response_delegate.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

// Records bridge responses so a test can assert how a dispatched action
// settled, and — unlike the sqlite fixture — ALSO records SendEvent so the
// onStatusChanged push is observable.
class RecordingDelegate : public NexusBridgeResponseDelegate {
 public:
  int success_count = 0;
  int error_count = 0;
  int event_count = 0;
  base::Value last_success;
  std::string last_error_code;
  base::Value last_event_payload;
  base::DictValue last_event_route;

  void SendSuccessResponse(const std::string& /*id*/,
                           const base::DictValue& /*route*/,
                           base::Value result) override {
    ++success_count;
    last_success = std::move(result);
  }
  void SendBinaryResponse(const std::string& /*id*/,
                          const base::DictValue& /*route*/,
                          base::Value /*result*/,
                          mojo_base::BigBuffer /*data*/,
                          std::string_view /*result_binary_field*/) override {}
  void SendErrorResponse(const std::string& /*id*/,
                         const base::DictValue& /*route*/,
                         const std::string& code,
                         const std::string& /*message*/) override {
    ++error_count;
    last_error_code = code;
  }
  void SendEvent(const base::DictValue& route, base::Value payload) override {
    ++event_count;
    last_event_route = route.Clone();
    last_event_payload = std::move(payload);
  }
};

base::DictValue RemoteAccessRoute(std::string_view action) {
  base::DictValue route;
  route.Set("namespace", "nexus");
  route.Set("capability", "remoteAccess");
  route.Set("action", std::string(action));
  return route;
}

class NexusRemoteAccessBridgeAdapterTest : public testing::Test {
 protected:
  content::BrowserTaskEnvironment task_environment_;
  RecordingDelegate delegate_;
};

// With no Profile the adapter can do nothing: dispatching either action
// through the registered handler table (the path a real bridge message takes)
// must answer the REMOTE_ACCESS_UNAVAILABLE error, and no onStatusChanged may
// be pushed. This pins the !profile_ branch and the dispatch/response shaping
// without needing a full browser (the service behind a real Profile is
// covered by the listener + router unit tests and the e2e suite).
TEST_F(NexusRemoteAccessBridgeAdapterTest,
       DispatchedActionsWithNoProfileError) {
  NexusRemoteAccessBridgeAdapter adapter(&delegate_, /*profile=*/nullptr);

  ASSERT_TRUE(adapter.Dispatch("getStatus", "1",
                               RemoteAccessRoute("getStatus"),
                               base::DictValue(), /*ports=*/{}));
  // BridgeResponseCallback delivers to the delegate via a posted task.
  task_environment_.RunUntilIdle();
  EXPECT_EQ(delegate_.error_count, 1);
  EXPECT_EQ(delegate_.last_error_code, "REMOTE_ACCESS_UNAVAILABLE");
  EXPECT_EQ(delegate_.success_count, 0);

  base::DictValue payload;
  payload.Set("enabled", true);
  ASSERT_TRUE(adapter.Dispatch("configure", "2",
                               RemoteAccessRoute("configure"),
                               std::move(payload), /*ports=*/{}));
  task_environment_.RunUntilIdle();
  EXPECT_EQ(delegate_.error_count, 2);
  EXPECT_EQ(delegate_.last_error_code, "REMOTE_ACCESS_UNAVAILABLE");

  // No onStatusChanged without a service behind the adapter.
  EXPECT_EQ(delegate_.event_count, 0);
}

// The four action names the runtime calls must all be registered at
// construction (the manifest-advertised surface); an unknown action must NOT
// dispatch.
TEST_F(NexusRemoteAccessBridgeAdapterTest, RegistersExactlyTheFourActions) {
  NexusRemoteAccessBridgeAdapter adapter(&delegate_, /*profile=*/nullptr);
  EXPECT_TRUE(adapter.Dispatch("getStatus", "1",
                               RemoteAccessRoute("getStatus"),
                               base::DictValue(), {}));
  EXPECT_TRUE(adapter.Dispatch("configure", "2",
                               RemoteAccessRoute("configure"),
                               base::DictValue(), {}));
  EXPECT_TRUE(adapter.Dispatch("getPairings", "3",
                               RemoteAccessRoute("getPairings"),
                               base::DictValue(), {}));
  base::DictValue set_payload;
  set_payload.Set("pairings", base::ListValue());
  EXPECT_TRUE(adapter.Dispatch("setPairings", "4",
                               RemoteAccessRoute("setPairings"),
                               std::move(set_payload), {}));
  delegate_.error_count = 0;
  EXPECT_FALSE(adapter.Dispatch("noSuchAction", "5",
                                RemoteAccessRoute("noSuchAction"),
                                base::DictValue(), {}));
  // The unknown action never reached a handler → no error shaped.
  EXPECT_EQ(delegate_.error_count, 0);
}

// The two pairing actions hit the same !profile_ guard as the status ones:
// with no attached Profile both must answer REMOTE_ACCESS_UNAVAILABLE.
TEST_F(NexusRemoteAccessBridgeAdapterTest,
       PairingActionsWithNoProfileError) {
  NexusRemoteAccessBridgeAdapter adapter(&delegate_, /*profile=*/nullptr);

  ASSERT_TRUE(adapter.Dispatch("getPairings", "1",
                               RemoteAccessRoute("getPairings"),
                               base::DictValue(), {}));
  task_environment_.RunUntilIdle();
  EXPECT_EQ(delegate_.error_count, 1);
  EXPECT_EQ(delegate_.last_error_code, "REMOTE_ACCESS_UNAVAILABLE");

  base::DictValue payload;
  payload.Set("pairings", base::ListValue());
  ASSERT_TRUE(adapter.Dispatch("setPairings", "2",
                               RemoteAccessRoute("setPairings"),
                               std::move(payload), {}));
  task_environment_.RunUntilIdle();
  EXPECT_EQ(delegate_.error_count, 2);
  EXPECT_EQ(delegate_.last_error_code, "REMOTE_ACCESS_UNAVAILABLE");
  EXPECT_EQ(delegate_.success_count, 0);
}

}  // namespace
}  // namespace nexus
