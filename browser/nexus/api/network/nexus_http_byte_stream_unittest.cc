// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/network/nexus_http_byte_stream.h"

#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "net/base/host_port_pair.h"
#include "net/base/ip_address.h"
#include "net/base/ip_endpoint.h"
#include "net/base/proxy_chain.h"
#include "net/base/proxy_server.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "services/network/public/cpp/weak_wrapper_shared_url_loader_factory.h"
#include "services/network/test/test_url_loader_factory.h"
#include "services/network/test/test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace nexus {
namespace {

constexpr char kUrl[] = "https://files.example.com/big.bin";

class NexusHttpByteStreamTest : public testing::Test {
 protected:
  // Serve kUrl from `address` (directly, or through a proxy).
  void Serve(const net::IPAddress& address,
             std::string body,
             bool via_proxy = false) {
    auto head = network::CreateURLResponseHead(net::HTTP_OK);
    head->remote_endpoint = net::IPEndPoint(address, 443);
    head->proxy_chain =
        via_proxy ? net::ProxyChain(net::ProxyServer::SCHEME_HTTP,
                                    net::HostPortPair("127.0.0.1", 7890))
                  : net::ProxyChain::Direct();
    factory_.AddResponse(GURL(kUrl), std::move(head), body,
                         network::URLLoaderCompletionStatus(net::OK));
  }

  std::unique_ptr<NexusHttpByteStream> MakeStream(bool egress_policy) {
    auto request = std::make_unique<network::ResourceRequest>();
    request->url = GURL(kUrl);
    request->method = "GET";
    auto stream = std::make_unique<NexusHttpByteStream>(
        factory_.GetSafeWeakWrapper(), std::move(request), std::string(),
        std::string("application/octet-stream"),
        std::numeric_limits<size_t>::max(), base::Seconds(30),
        NexusHttpByteStream::RedirectCheck(), std::vector<std::string>(),
        NexusHttpByteStream::BodyTransform(), TRAFFIC_ANNOTATION_FOR_TESTS);
    if (egress_policy) {
      stream->EnableEgressPolicy();
    }
    return stream;
  }

  NexusHttpByteStream::OpenResult Open(NexusHttpByteStream* stream) {
    std::optional<NexusHttpByteStream::OpenResult> result;
    stream->Start(base::BindOnce(
        [](std::optional<NexusHttpByteStream::OpenResult>* out,
           NexusHttpByteStream::OpenResult r) { *out = std::move(r); },
        &result));
    task_environment_.RunUntilIdle();
    EXPECT_TRUE(result.has_value());
    return result ? std::move(*result) : NexusHttpByteStream::OpenResult();
  }

  NexusHttpByteStream::ReadResult Read(NexusHttpByteStream* stream) {
    std::optional<NexusHttpByteStream::ReadResult> result;
    stream->Read(base::TimeDelta(), 1 << 20,
                 base::BindOnce(
                     [](std::optional<NexusHttpByteStream::ReadResult>* out,
                        NexusHttpByteStream::ReadResult r) {
                       *out = std::move(r);
                     },
                     &result));
    task_environment_.RunUntilIdle();
    EXPECT_TRUE(result.has_value());
    return result ? std::move(*result) : NexusHttpByteStream::ReadResult();
  }

  base::test::SingleThreadTaskEnvironment task_environment_;
  network::TestURLLoaderFactory factory_;
};

TEST_F(NexusHttpByteStreamTest, APrivatePeerIsRefusedUnderThePolicy) {
  Serve(net::IPAddress(192, 168, 1, 10), "router admin page");
  auto stream = MakeStream(/*egress_policy=*/true);
  const NexusHttpByteStream::OpenResult open = Open(stream.get());
  EXPECT_EQ(open.error_code, NexusHttpByteStream::kEgressBlockedErrorCode);
  EXPECT_NE(open.error_message.find("192.168.1.10"), std::string::npos);
}

TEST_F(NexusHttpByteStreamTest, APublicPeerStreamsTheWholeBody) {
  const std::string body(3 * 1024 * 1024, 'x');  // past the old 5 MB-era caps
  Serve(net::IPAddress(93, 184, 216, 34), body);
  auto stream = MakeStream(/*egress_policy=*/true);
  const NexusHttpByteStream::OpenResult open = Open(stream.get());
  ASSERT_TRUE(open.error_code.empty()) << open.error_message;
  EXPECT_EQ(open.status, 200);
  std::string received;
  for (int i = 0; i < 16; ++i) {
    NexusHttpByteStream::ReadResult read = Read(stream.get());
    received += read.data;
    if (read.done) {
      EXPECT_TRUE(read.error_code.empty());
      break;
    }
  }
  EXPECT_EQ(received.size(), body.size());
}

// Through a proxy the peer is the proxy (often local): only the literal-host
// check (the adapter's) applies.
TEST_F(NexusHttpByteStreamTest, AProxiedResponseIsNotJudgedByItsPeer) {
  Serve(net::IPAddress(127, 0, 0, 1), "via proxy", /*via_proxy=*/true);
  auto stream = MakeStream(/*egress_policy=*/true);
  EXPECT_TRUE(Open(stream.get()).error_code.empty());
}

// Without the policy (the provider-scoped model.rawStream path) the peer is
// not judged at all.
TEST_F(NexusHttpByteStreamTest, ThePolicyIsOptIn) {
  Serve(net::IPAddress(10, 0, 0, 5), "lan");
  auto stream = MakeStream(/*egress_policy=*/false);
  const NexusHttpByteStream::OpenResult open = Open(stream.get());
  EXPECT_TRUE(open.error_code.empty()) << open.error_message;
  EXPECT_EQ(Read(stream.get()).data, "lan");
}

}  // namespace
}  // namespace nexus
