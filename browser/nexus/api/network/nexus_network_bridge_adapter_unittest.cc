// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/network/nexus_network_bridge_adapter.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "base/values.h"
#include "content/public/test/browser_task_environment.h"
#include "custom_browser/browser/nexus/nexus_bridge_response_delegate.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "net/base/ip_address.h"
#include "net/base/ip_endpoint.h"
#include "net/base/proxy_chain.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "services/network/public/cpp/weak_wrapper_shared_url_loader_factory.h"
#include "services/network/test/test_url_loader_factory.h"
#include "services/network/test/test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace nexus {
namespace {

class RecordingDelegate : public NexusBridgeResponseDelegate {
 public:
  int success_count = 0;
  int binary_count = 0;
  int error_count = 0;
  base::Value last_success;
  base::Value last_binary_envelope;
  std::string last_binary_bytes;
  std::string last_error_code;

  void SendSuccessResponse(const std::string& /*id*/,
                           const base::DictValue& /*route*/,
                           base::Value result) override {
    ++success_count;
    last_success = std::move(result);
  }
  void SendBinaryResponse(const std::string& /*id*/,
                          const base::DictValue& /*route*/,
                          base::Value result,
                          mojo_base::BigBuffer data,
                          std::string_view /*result_binary_field*/) override {
    ++binary_count;
    last_binary_envelope = std::move(result);
    last_binary_bytes.assign(reinterpret_cast<const char*>(data.data()),
                             data.size());
  }
  void SendErrorResponse(const std::string& /*id*/,
                         const base::DictValue& /*route*/,
                         const std::string& code,
                         const std::string& /*message*/) override {
    ++error_count;
    last_error_code = code;
  }
  void SendEvent(const base::DictValue& /*route*/,
                 base::Value /*payload*/) override {}
};

base::DictValue NetworkRoute(std::string_view action) {
  base::DictValue route;
  route.Set("namespace", "nexus");
  route.Set("capability", "network");
  route.Set("action", std::string(action));
  return route;
}

class NexusNetworkStreamTest : public testing::Test {
 protected:
  NexusNetworkStreamTest() {
    adapter_.SetURLLoaderFactoryForTesting(factory_.GetSafeWeakWrapper());
  }

  void Dispatch(std::string_view action, base::DictValue payload) {
    EXPECT_TRUE(adapter_.Dispatch(action, "req", NetworkRoute(action),
                                  payload, {}));
    task_environment_.RunUntilIdle();
  }

  static base::DictValue OpenPayload(const std::string& url) {
    base::DictValue payload;
    payload.Set("url", url);
    // A `bytes` input that is empty inline is read from a transferred port
    // instead (the guest sends an empty body that way); a test without a
    // port sends one inline byte.
    payload.Set("method", "POST");
    base::ListValue body;
    body.Append(120);
    payload.Set("body", std::move(body));
    payload.Set("streamId", "stream-0001");
    return payload;
  }

  content::BrowserTaskEnvironment task_environment_;
  network::TestURLLoaderFactory factory_;
  RecordingDelegate delegate_;
  NexusNetworkBridgeAdapter adapter_{&delegate_, nullptr};
};

TEST_F(NexusNetworkStreamTest, OpenReadDoneOverThePublicInternet) {
  const GURL url("https://api.example.com/events");
  auto head = network::CreateURLResponseHead(net::HTTP_OK);
  head->remote_endpoint = net::IPEndPoint(net::IPAddress(93, 184, 216, 34), 443);
  head->proxy_chain = net::ProxyChain::Direct();
  factory_.AddResponse(url, std::move(head), "data: hi\n\n",
                       network::URLLoaderCompletionStatus(net::OK));

  Dispatch("streamOpen", OpenPayload(url.spec()));
  ASSERT_EQ(delegate_.success_count, 1) << delegate_.last_error_code;
  EXPECT_EQ(*delegate_.last_success.GetDict().FindString("streamId"),
            "stream-0001");
  EXPECT_EQ(delegate_.last_success.GetDict().FindInt("status"), 200);

  base::DictValue read;
  read.Set("streamId", "stream-0001");
  Dispatch("streamRead", std::move(read));
  ASSERT_EQ(delegate_.binary_count, 1);
  EXPECT_EQ(delegate_.last_binary_bytes, "data: hi\n\n");
  EXPECT_EQ(delegate_.last_binary_envelope.GetDict().FindBool("done"), true);
  // `done` erases the stream.
  EXPECT_EQ(adapter_.stream_count_for_testing(), 0u);
}

TEST_F(NexusNetworkStreamTest, ALiteralPrivateHostIsRefusedBeforeDialing) {
  Dispatch("streamOpen", OpenPayload("http://127.0.0.1:8080/admin"));
  EXPECT_EQ(delegate_.last_error_code, "EGRESS_BLOCKED");
  EXPECT_EQ(adapter_.stream_count_for_testing(), 0u);
}

TEST_F(NexusNetworkStreamTest, ReservedHeadersAreRefused) {
  base::DictValue payload = OpenPayload("https://api.example.com/x");
  base::DictValue headers;
  headers.Set("Cookie", "session=1");
  payload.Set("headers", std::move(headers));
  Dispatch("streamOpen", std::move(payload));
  EXPECT_EQ(delegate_.last_error_code, "INVALID_ARGS");
}

TEST_F(NexusNetworkStreamTest, CancelBeforeOpenRefusesTheOpen) {
  base::DictValue cancel;
  cancel.Set("streamId", "stream-0001");
  Dispatch("streamCancel", std::move(cancel));
  EXPECT_EQ(delegate_.success_count, 1);
  Dispatch("streamOpen", OpenPayload("https://api.example.com/late"));
  EXPECT_EQ(delegate_.last_error_code, "CANCELLED");
}

TEST_F(NexusNetworkStreamTest, ClosingTheConnectionCancelsOpenStreams) {
  // No response registered: the open stays pending on the head.
  Dispatch("streamOpen", OpenPayload("https://api.example.com/slow"));
  EXPECT_EQ(adapter_.stream_count_for_testing(), 1u);
  adapter_.OnBridgeConnectionClosed();
  EXPECT_EQ(adapter_.stream_count_for_testing(), 0u);
}

}  // namespace
}  // namespace nexus
