// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/network/nexus_websocket_connection.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "net/base/ip_address.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_errors.h"
#include "net/proxy_resolution/proxy_info.h"
#include "services/network/test/test_network_context.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace nexus {
namespace {

// The server end the network service would hold.
class FakeWebSocket : public network::mojom::WebSocket {
 public:
  void SendMessage(network::mojom::WebSocketMessageType type,
                   uint64_t data_length) override {
    sent.emplace_back(type, data_length);
  }
  void StartReceiving() override { receiving = true; }
  void StartClosingHandshake(uint16_t code,
                             const std::string& reason) override {
    close_code = code;
    close_reason = reason;
  }

  std::vector<std::pair<network::mojom::WebSocketMessageType, uint64_t>> sent;
  bool receiving = false;
  std::optional<uint16_t> close_code;
  std::string close_reason;
};

// A NetworkContext that answers the proxy lookup and parks the handshake
// client, so the test plays the network service.
class FakeNetworkContext : public network::TestNetworkContext {
 public:
  void LookUpProxyForURL(
      const GURL& url,
      const net::NetworkAnonymizationKey& /*network_anonymization_key*/,
      mojo::PendingRemote<network::mojom::ProxyLookupClient> client)
      override {
    looked_up = url;
    lookup_client.Bind(std::move(client));
    net::ProxyInfo info;
    if (via_proxy) {
      info.UseNamedProxy("127.0.0.1:7890");
    } else {
      info.UseDirect();
    }
    lookup_client->OnProxyLookupComplete(net::OK, info);
  }

  void CreateWebSocket(
      const GURL& url,
      const std::vector<std::string>& requested_protocols,
      net::StorageAccessApiStatus /*storage_access_api_status*/,
      const net::IsolationInfo& /*isolation_info*/,
      std::vector<network::mojom::HttpHeaderPtr> additional_headers,
      const network::OriginatingProcessId& /*process_id*/,
      const url::Origin& /*origin*/,
      network::mojom::ClientSecurityStatePtr /*client_security_state*/,
      uint32_t options,
      const net::MutableNetworkTrafficAnnotationTag& /*traffic_annotation*/,
      mojo::PendingRemote<network::mojom::WebSocketHandshakeClient>
          handshake_client,
      mojo::PendingRemote<network::mojom::URLLoaderNetworkServiceObserver>
      /*url_loader_network_observer*/,
      mojo::PendingRemote<network::mojom::WebSocketAuthenticationHandler>
      /*auth_handler*/,
      mojo::PendingRemote<network::mojom::TrustedHeaderClient>
      /*header_client*/,
      const std::optional<base::UnguessableToken>& /*throttling_profile_id*/,
      const std::optional<base::UnguessableToken>& /*network_restrictions_id*/)
      override {
    created_url = url;
    protocols = requested_protocols;
    headers = std::move(additional_headers);
    created_options = options;
    handshake.Bind(std::move(handshake_client));
  }

  bool via_proxy = false;
  GURL looked_up;
  GURL created_url;
  std::vector<std::string> protocols;
  std::vector<network::mojom::HttpHeaderPtr> headers;
  uint32_t created_options = 0;
  mojo::Remote<network::mojom::ProxyLookupClient> lookup_client;
  mojo::Remote<network::mojom::WebSocketHandshakeClient> handshake;
};

class NexusWebSocketConnectionTest : public testing::Test {
 protected:
  void Open(bool egress_policy) {
    std::vector<network::mojom::HttpHeaderPtr> headers;
    headers.push_back(network::mojom::HttpHeader::New("X-Test", "1"));
    connection_ = std::make_unique<NexusWebSocketConnection>(
        &context_, GURL("wss://rt.example.com/socket"),
        std::vector<std::string>{"chat"}, std::move(headers), egress_policy,
        base::BindOnce(
            [](NexusWebSocketConnectionTest* self, std::string code,
               std::string message, std::string protocol) {
              self->open_code_ = code;
              self->open_protocol_ = protocol;
              self->opened_ = true;
            },
            base::Unretained(this)),
        base::BindRepeating(
            [](NexusWebSocketConnectionTest* self, bool binary,
               std::string data) {
              self->messages_.emplace_back(binary, std::move(data));
            },
            base::Unretained(this)),
        base::BindOnce(
            [](NexusWebSocketConnectionTest* self, uint16_t code,
               std::string reason, bool was_clean) {
              self->closed_code_ = code;
            },
            base::Unretained(this)));
    connection_->Start();
    task_environment_.RunUntilIdle();
  }

  // Plays the network service completing the handshake with `peer`.
  void Establish(const net::IPAddress& peer) {
    ASSERT_TRUE(context_.handshake.is_bound());
    ASSERT_EQ(mojo::CreateDataPipe(nullptr, server_writes_, readable_),
              MOJO_RESULT_OK);
    ASSERT_EQ(mojo::CreateDataPipe(nullptr, writable_, server_reads_),
              MOJO_RESULT_OK);
    auto response = network::mojom::WebSocketHandshakeResponse::New();
    response->remote_endpoint = net::IPEndPoint(peer, 443);
    response->selected_protocol = "chat";
    context_.handshake->OnConnectionEstablished(
        socket_receiver_.BindNewPipeAndPassRemote(),
        client_.BindNewPipeAndPassReceiver(), std::move(response),
        std::move(readable_), std::move(writable_));
    task_environment_.RunUntilIdle();
  }

  base::test::SingleThreadTaskEnvironment task_environment_;
  FakeNetworkContext context_;
  FakeWebSocket socket_;
  mojo::Receiver<network::mojom::WebSocket> socket_receiver_{&socket_};
  mojo::Remote<network::mojom::WebSocketClient> client_;
  mojo::ScopedDataPipeProducerHandle server_writes_;
  mojo::ScopedDataPipeConsumerHandle readable_;
  mojo::ScopedDataPipeProducerHandle writable_;
  mojo::ScopedDataPipeConsumerHandle server_reads_;
  std::unique_ptr<NexusWebSocketConnection> connection_;

  bool opened_ = false;
  std::string open_code_;
  std::string open_protocol_;
  std::vector<std::pair<bool, std::string>> messages_;
  std::optional<uint16_t> closed_code_;
};

TEST_F(NexusWebSocketConnectionTest, OpensExchangesMessagesAndCloses) {
  Open(/*egress_policy=*/true);
  EXPECT_EQ(context_.looked_up, GURL("https://rt.example.com/socket"));
  EXPECT_EQ(context_.created_url, GURL("wss://rt.example.com/socket"));
  EXPECT_EQ(context_.protocols, std::vector<std::string>{"chat"});
  ASSERT_EQ(context_.headers.size(), 1u);
  EXPECT_NE(context_.created_options &
                network::mojom::kWebSocketOptionBlockAllCookies,
            0u);

  Establish(net::IPAddress(93, 184, 216, 34));
  ASSERT_TRUE(opened_);
  EXPECT_TRUE(open_code_.empty());
  EXPECT_EQ(open_protocol_, "chat");
  EXPECT_TRUE(socket_.receiving);

  // A text message split over two frames is delivered whole.
  const std::string incoming = "hello world";
  ASSERT_EQ(server_writes_->WriteAllData(base::as_byte_span(incoming)),
            MOJO_RESULT_OK);
  client_->OnDataFrame(false, network::mojom::WebSocketMessageType::TEXT, 6);
  client_->OnDataFrame(true,
                       network::mojom::WebSocketMessageType::CONTINUATION, 5);
  task_environment_.RunUntilIdle();
  ASSERT_EQ(messages_.size(), 1u);
  EXPECT_FALSE(messages_[0].first);
  EXPECT_EQ(messages_[0].second, "hello world");

  // A send announces the message, then its bytes follow on the pipe.
  EXPECT_TRUE(connection_->Send(/*binary=*/true, "\x01\x02"));
  task_environment_.RunUntilIdle();
  ASSERT_EQ(socket_.sent.size(), 1u);
  EXPECT_EQ(socket_.sent[0].first, network::mojom::WebSocketMessageType::BINARY);
  EXPECT_EQ(socket_.sent[0].second, 2u);
  std::string written(2, '\0');
  size_t read = 0;
  ASSERT_EQ(server_reads_->ReadData(MOJO_READ_DATA_FLAG_NONE,
                                    base::as_writable_byte_span(written), read),
            MOJO_RESULT_OK);
  EXPECT_EQ(written, "\x01\x02");

  connection_->Close(1000, "bye");
  task_environment_.RunUntilIdle();
  EXPECT_EQ(socket_.close_code, 1000);
  client_->OnDropChannel(true, 1000, "bye");
  task_environment_.RunUntilIdle();
  EXPECT_EQ(closed_code_, 1000);
  EXPECT_FALSE(connection_->Send(false, "late"));
}

// A message past the cap closes with 1009, and its bytes are still taken off
// the pipe: left there, the next frame would read them as its own data (a
// corrupted message) and the network service, its pipe full, would stall.
// After that no message is delivered — the socket is closing.
TEST_F(NexusWebSocketConnectionTest, AnOversizedMessageIsDrainedNotDelivered) {
  Open(/*egress_policy=*/true);
  Establish(net::IPAddress(93, 184, 216, 34));
  ASSERT_TRUE(opened_);

  const size_t oversized = NexusWebSocketConnection::kMaxMessageBytes + 1;
  client_->OnDataFrame(true, network::mojom::WebSocketMessageType::TEXT,
                       oversized);
  client_->OnDataFrame(true, network::mojom::WebSocketMessageType::TEXT, 2);
  task_environment_.RunUntilIdle();
  EXPECT_EQ(socket_.close_code, 1009);

  const std::string chunk(64 * 1024, 'x');
  size_t left = oversized;
  for (int i = 0; left > 0 && i < 4096; ++i) {
    size_t written = 0;
    const MojoResult result = server_writes_->WriteData(
        base::as_byte_span(chunk).first(std::min(left, chunk.size())),
        MOJO_WRITE_DATA_FLAG_NONE, written);
    ASSERT_TRUE(result == MOJO_RESULT_OK || result == MOJO_RESULT_SHOULD_WAIT);
    if (result == MOJO_RESULT_OK) {
      left -= written;
    }
    task_environment_.RunUntilIdle();
  }
  ASSERT_EQ(left, 0u) << "the oversized frame was left on the pipe";
  ASSERT_EQ(server_writes_->WriteAllData(base::as_byte_span(std::string("ok"))),
            MOJO_RESULT_OK);
  task_environment_.RunUntilIdle();
  EXPECT_TRUE(messages_.empty());
}

TEST_F(NexusWebSocketConnectionTest, APrivatePeerIsRefusedWhenDirect) {
  Open(/*egress_policy=*/true);
  Establish(net::IPAddress(10, 1, 2, 3));
  ASSERT_TRUE(opened_);
  EXPECT_EQ(open_code_, NexusWebSocketConnection::kEgressBlockedErrorCode);
  EXPECT_FALSE(connection_->is_open());
}

TEST_F(NexusWebSocketConnectionTest, ThroughAProxyThePeerIsNotJudged) {
  context_.via_proxy = true;
  Open(/*egress_policy=*/true);
  Establish(net::IPAddress(127, 0, 0, 1));
  EXPECT_TRUE(open_code_.empty());
  EXPECT_TRUE(connection_->is_open());
}

TEST_F(NexusWebSocketConnectionTest, WithoutThePolicyThereIsNoLookup) {
  Open(/*egress_policy=*/false);
  EXPECT_FALSE(context_.looked_up.is_valid());
  Establish(net::IPAddress(192, 168, 0, 2));
  EXPECT_TRUE(open_code_.empty());
}

// The handshake must never reach a LAN / loopback peer: the network service
// is asked to refuse such a socket before anything is written, and that
// refusal reads as the egress policy's.
TEST_F(NexusWebSocketConnectionTest, ALocalPeerIsRefusedBeforeTheHandshake) {
  Open(/*egress_policy=*/true);
  EXPECT_NE(context_.created_options &
                network::mojom::kURLLoadOptionBlockLocalRequest,
            0u);
  EXPECT_NE(context_.created_options &
                network::mojom::kWebSocketOptionBlockAllCookies,
            0u);
  context_.handshake->OnFailure(
      "blocked", net::ERR_BLOCKED_BY_LOCAL_NETWORK_ACCESS_CHECKS, 0);
  task_environment_.RunUntilIdle();
  EXPECT_EQ(open_code_, NexusWebSocketConnection::kEgressBlockedErrorCode);
}

TEST_F(NexusWebSocketConnectionTest, OptedOutSocketsMayReachTheLan) {
  Open(/*egress_policy=*/false);
  EXPECT_EQ(context_.created_options &
                network::mojom::kURLLoadOptionBlockLocalRequest,
            0u);
}

TEST_F(NexusWebSocketConnectionTest, AHandshakeFailureFailsTheOpen) {
  Open(/*egress_policy=*/false);
  context_.handshake->OnFailure("refused", net::ERR_CONNECTION_REFUSED, 0);
  task_environment_.RunUntilIdle();
  EXPECT_EQ(open_code_, "WS_CONNECT_FAILED");
}

}  // namespace
}  // namespace nexus
