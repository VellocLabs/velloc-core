// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_NETWORK_NEXUS_WEBSOCKET_CONNECTION_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_NETWORK_NEXUS_WEBSOCKET_CONNECTION_H_

#include <stddef.h>
#include <stdint.h>

#include <optional>
#include <string>
#include <vector>

#include "base/containers/circular_deque.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "mojo/public/cpp/system/simple_watcher.h"
#include "services/network/public/mojom/proxy_lookup_client.mojom.h"
#include "services/network/public/mojom/websocket.mojom.h"
#include "url/gurl.h"

namespace net {
class ProxyInfo;
}

namespace network::mojom {
class NetworkContext;
}

namespace nexus {

// One `network.wsOpen` WebSocket via NetworkContext::CreateWebSocket; cookies
// are never sent. Egress policy (unless opted out): IP literals refused by the
// adapter, loopback / private peers refused at connect, the handshake peer
// judged again unless proxied. Messages are delivered whole (at most
// kMaxMessageBytes, else close 1009). Lifecycle: Start() → `open` once →
// messages → `closed` once; deleting it drops the socket. UI thread.
class NexusWebSocketConnection : public network::mojom::ProxyLookupClient,
                                 public network::mojom::WebSocketHandshakeClient,
                                 public network::mojom::WebSocketClient {
 public:
  // error_code empty on success; `protocol` is the server-selected one.
  using OpenCallback =
      base::OnceCallback<void(std::string error_code,
                              std::string error_message,
                              std::string protocol)>;
  using MessageCallback =
      base::RepeatingCallback<void(bool binary, std::string data)>;
  using ClosedCallback = base::OnceCallback<
      void(uint16_t code, std::string reason, bool was_clean)>;

  static constexpr size_t kMaxMessageBytes = 16 * 1024 * 1024;
  static constexpr char kEgressBlockedErrorCode[] = "EGRESS_BLOCKED";

  NexusWebSocketConnection(
      network::mojom::NetworkContext* network_context,
      const GURL& url,
      std::vector<std::string> protocols,
      std::vector<network::mojom::HttpHeaderPtr> headers,
      bool egress_policy,
      OpenCallback on_open,
      MessageCallback on_message,
      ClosedCallback on_closed);
  ~NexusWebSocketConnection() override;

  NexusWebSocketConnection(const NexusWebSocketConnection&) = delete;
  NexusWebSocketConnection& operator=(const NexusWebSocketConnection&) =
      delete;

  void Start();

  // Queues one message. False when the socket is not open.
  bool Send(bool binary, std::string data);

  // Starts the closing handshake; `closed` fires when it completes.
  void Close(uint16_t code, const std::string& reason);

  bool is_open() const { return state_ == State::kOpen; }

  // network::mojom::ProxyLookupClient:
  void OnProxyLookupComplete(
      int32_t net_error,
      const std::optional<net::ProxyInfo>& proxy_info) override;

  // network::mojom::WebSocketHandshakeClient:
  void OnOpeningHandshakeStarted(
      network::mojom::WebSocketHandshakeRequestPtr request) override;
  void OnFailure(const std::string& message,
                 int net_error,
                 int response_code) override;
  void OnConnectionEstablished(
      mojo::PendingRemote<network::mojom::WebSocket> socket,
      mojo::PendingReceiver<network::mojom::WebSocketClient> client_receiver,
      network::mojom::WebSocketHandshakeResponsePtr response,
      mojo::ScopedDataPipeConsumerHandle readable,
      mojo::ScopedDataPipeProducerHandle writable) override;

  // network::mojom::WebSocketClient:
  void OnDataFrame(bool fin,
                   network::mojom::WebSocketMessageType type,
                   uint64_t data_length) override;
  void OnDropChannel(bool was_clean,
                     uint16_t code,
                     const std::string& reason) override;
  void OnClosingHandshake() override;

 private:
  enum class State { kIdle, kLookingUpProxy, kConnecting, kOpen, kClosed };

  void Connect();
  void FailOpen(std::string code, std::string message);
  void ReadFrame(MojoResult result, const mojo::HandleSignalsState& state);
  void FinishMessage();
  void PumpWrites(MojoResult result, const mojo::HandleSignalsState& state);
  void Drop(uint16_t code, std::string reason, bool was_clean);
  void OnPipeDisconnected();

  raw_ptr<network::mojom::NetworkContext> network_context_;
  const GURL url_;
  std::vector<std::string> protocols_;
  std::vector<network::mojom::HttpHeaderPtr> headers_;
  const bool egress_policy_;
  // Whether the proxy lookup found a direct connection (the peer can then be
  // judged). False until the lookup answers; true when the policy is off.
  bool direct_ = false;

  OpenCallback on_open_;
  MessageCallback on_message_;
  ClosedCallback on_closed_;
  State state_ = State::kIdle;

  // The message being received: its type (from its first frame) and bytes.
  bool message_binary_ = false;
  std::string message_;
  size_t frame_remaining_ = 0;
  bool frame_fin_ = false;
  // Set once a message ran past kMaxMessageBytes and the socket began
  // closing with 1009: from then on every frame's bytes are read off the
  // pipe and dropped, so no later frame reads them as its own and no message
  // is delivered.
  bool discarding_ = false;

  // Messages being sent: each one's bytes follow its SendMessage on the pipe.
  base::circular_deque<std::string> write_queue_;
  size_t write_offset_ = 0;

  mojo::Receiver<network::mojom::ProxyLookupClient> proxy_receiver_{this};
  mojo::Receiver<network::mojom::WebSocketHandshakeClient> handshake_receiver_{
      this};
  mojo::Receiver<network::mojom::WebSocketClient> client_receiver_{this};
  mojo::Remote<network::mojom::WebSocket> websocket_;
  mojo::ScopedDataPipeConsumerHandle readable_;
  mojo::SimpleWatcher readable_watcher_;
  mojo::ScopedDataPipeProducerHandle writable_;
  mojo::SimpleWatcher writable_watcher_;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<NexusWebSocketConnection> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_NETWORK_NEXUS_WEBSOCKET_CONNECTION_H_
