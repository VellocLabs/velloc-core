// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/network/nexus_websocket_connection.h"

#include <algorithm>
#include <utility>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/strings/stringprintf.h"
#include "base/task/sequenced_task_runner.h"
#include "custom_browser/browser/nexus/api/network/network_egress_policy.h"
#include "net/base/isolation_info.h"
#include "net/base/net_errors.h"
#include "net/base/network_anonymization_key.h"
#include "net/proxy_resolution/proxy_info.h"
#include "net/storage_access_api/status.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/originating_process_id.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "url/origin.h"

namespace nexus {

namespace {

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("nexus_network_websocket", R"(
      semantics {
        sender: "Nexus AI Agent Platform"
        description:
          "A WebSocket the Nexus web runtime opens through the browser's "
          "network stack (network.wsOpen): a model provider's realtime "
          "API, an MCP server or a plugin endpoint the user configured. "
          "Only publicly routable hosts are reached "
          "(network_egress_policy.h) unless the person opted a socket out."
        trigger:
          "The AI assistant or a feature the user enabled opens a socket."
        data:
          "The messages the runtime sends, and any secret the user stored "
          "for the socket's origin."
        destination: OTHER
        internal {
          contacts {
            email: "niclas@niclas.cc"
          }
        }
        user_data {
          type: USER_CONTENT
        }
        last_reviewed: "2026-10-04"
      }
      policy {
        cookies_allowed: NO
        setting:
          "Users configure providers, plugins and MCP servers in the "
          "assistant settings."
        policy_exception_justification:
          "Not yet covered by enterprise policy."
      }
    )");

// The http(s) form of a ws(s) URL — what the proxy configuration is keyed on.
GURL ProxyLookupUrl(const GURL& url) {
  GURL::Replacements replacements;
  replacements.SetSchemeStr(url.SchemeIs("wss") ? "https" : "http");
  return url.ReplaceComponents(replacements);
}

}  // namespace

NexusWebSocketConnection::NexusWebSocketConnection(
    network::mojom::NetworkContext* network_context,
    const GURL& url,
    std::vector<std::string> protocols,
    std::vector<network::mojom::HttpHeaderPtr> headers,
    bool egress_policy,
    OpenCallback on_open,
    MessageCallback on_message,
    ClosedCallback on_closed)
    : network_context_(network_context),
      url_(url),
      protocols_(std::move(protocols)),
      headers_(std::move(headers)),
      egress_policy_(egress_policy),
      on_open_(std::move(on_open)),
      on_message_(std::move(on_message)),
      on_closed_(std::move(on_closed)),
      readable_watcher_(FROM_HERE, mojo::SimpleWatcher::ArmingPolicy::MANUAL),
      writable_watcher_(FROM_HERE,
                        mojo::SimpleWatcher::ArmingPolicy::MANUAL) {}

NexusWebSocketConnection::~NexusWebSocketConnection() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

void NexusWebSocketConnection::Start() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!egress_policy_) {
    direct_ = true;
    Connect();
    return;
  }
  // Through a proxy the handshake's peer is the proxy, not the origin, so
  // whether the peer can be judged is decided by the proxy configuration.
  state_ = State::kLookingUpProxy;
  mojo::PendingRemote<network::mojom::ProxyLookupClient> client =
      proxy_receiver_.BindNewPipeAndPassRemote();
  proxy_receiver_.set_disconnect_handler(base::BindOnce(
      &NexusWebSocketConnection::OnProxyLookupComplete,
      base::Unretained(this), net::ERR_ABORTED, std::nullopt));
  network_context_->LookUpProxyForURL(
      ProxyLookupUrl(url_), net::NetworkAnonymizationKey::CreateTransient(),
      std::move(client));
}

void NexusWebSocketConnection::OnProxyLookupComplete(
    int32_t /*net_error*/,
    const std::optional<net::ProxyInfo>& proxy_info) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ != State::kLookingUpProxy) {
    return;
  }
  proxy_receiver_.reset();
  // A failed lookup is treated as "maybe a proxy" (the peer is not judged):
  // the literal-host check already ran, and refusing every socket on a
  // lookup hiccup would be worse than the narrow gap.
  direct_ = proxy_info.has_value() && proxy_info->is_direct();
  Connect();
}

void NexusWebSocketConnection::Connect() {
  state_ = State::kConnecting;
  mojo::PendingRemote<network::mojom::WebSocketHandshakeClient> handshake =
      handshake_receiver_.BindNewPipeAndPassRemote();
  handshake_receiver_.set_disconnect_handler(base::BindOnce(
      &NexusWebSocketConnection::OnPipeDisconnected, base::Unretained(this)));
  const url::Origin origin = url::Origin::Create(url_);
  // Pre-send egress enforcement, as for http (network_egress_policy.h): the
  // network service refuses a socket whose peer is loopback or private when
  // it connects, before the opening handshake (and any expanded
  // `{{secret:NAME}}` header) is written; the open fails with
  // ERR_BLOCKED_BY_LOCAL_NETWORK_ACCESS_CHECKS (OnFailure). There is no
  // WebSocket option for it: services/network/websocket.cc hands these
  // options to LocalNetworkAccessChecker as URL-load options, so the URL-load
  // bit is the switch. Through a proxy the peer is the proxy and the check
  // passes (an unknown address space counts as public), as for http.
  // OnConnectionEstablished still judges the peer for the ranges Chromium
  // counts as public.
  static_assert((network::mojom::kURLLoadOptionBlockLocalRequest &
                 (network::mojom::kWebSocketOptionBlockAllCookies |
                  network::mojom::kWebSocketOptionBlockThirdPartyCookies)) ==
                0);
  uint32_t options = network::mojom::kWebSocketOptionBlockAllCookies;
  if (egress_policy_) {
    options |= network::mojom::kURLLoadOptionBlockLocalRequest;
  }
  network_context_->CreateWebSocket(
      url_, protocols_, net::StorageAccessApiStatus::kNone,
      net::IsolationInfo::CreateForInternalRequest(origin),
      std::move(headers_), network::OriginatingProcessId::browser(), origin,
      network::mojom::ClientSecurityState::New(), options,
      net::MutableNetworkTrafficAnnotationTag(kTrafficAnnotation),
      std::move(handshake),
      /*url_loader_network_observer=*/mojo::NullRemote(),
      /*auth_handler=*/mojo::NullRemote(),
      /*header_client=*/mojo::NullRemote(),
      /*throttling_profile_id=*/std::nullopt,
      /*network_restrictions_id=*/std::nullopt);
}

void NexusWebSocketConnection::OnOpeningHandshakeStarted(
    network::mojom::WebSocketHandshakeRequestPtr /*request*/) {}

void NexusWebSocketConnection::OnFailure(const std::string& message,
                                         int net_error,
                                         int response_code) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (egress_policy_) {
    if (std::optional<std::string> refusal =
            EgressRefusalForNetError(net_error, url_)) {
      FailOpen(kEgressBlockedErrorCode,
               "Refused by the egress policy: " + *refusal + ".");
      return;
    }
  }
  FailOpen("WS_CONNECT_FAILED",
           base::StringPrintf("%s (net error %d, HTTP %d)", message.c_str(),
                              net_error, response_code));
}

void NexusWebSocketConnection::OnConnectionEstablished(
    mojo::PendingRemote<network::mojom::WebSocket> socket,
    mojo::PendingReceiver<network::mojom::WebSocketClient> client_receiver,
    network::mojom::WebSocketHandshakeResponsePtr response,
    mojo::ScopedDataPipeConsumerHandle readable,
    mojo::ScopedDataPipeProducerHandle writable) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ != State::kConnecting) {
    return;
  }
  if (egress_policy_ && direct_ && response) {
    const net::IPAddress& address = response->remote_endpoint.address();
    if (std::optional<std::string> refusal = EgressRefusalFor(address)) {
      // Dropping `socket` unopened closes the connection.
      FailOpen(kEgressBlockedErrorCode,
               base::StringPrintf("Refused by the egress policy: %s resolved "
                                  "to %s, a %s.",
                                  std::string(url_.host()).c_str(),
                                  address.ToString().c_str(),
                                  refusal->c_str()));
      return;
    }
  }

  websocket_.Bind(std::move(socket));
  readable_ = std::move(readable);
  writable_ = std::move(writable);
  readable_watcher_.Watch(
      readable_.get(), MOJO_HANDLE_SIGNAL_READABLE,
      MOJO_TRIGGER_CONDITION_SIGNALS_SATISFIED,
      base::BindRepeating(&NexusWebSocketConnection::ReadFrame,
                          base::Unretained(this)));
  writable_watcher_.Watch(
      writable_.get(), MOJO_HANDLE_SIGNAL_WRITABLE,
      MOJO_TRIGGER_CONDITION_SIGNALS_SATISFIED,
      base::BindRepeating(&NexusWebSocketConnection::PumpWrites,
                          base::Unretained(this)));
  client_receiver_.Bind(std::move(client_receiver));
  // The handshake pipe closes now; the client pipe is what reports a
  // network-process crash from here on.
  handshake_receiver_.set_disconnect_handler(base::DoNothing());
  client_receiver_.set_disconnect_handler(base::BindOnce(
      &NexusWebSocketConnection::OnPipeDisconnected, base::Unretained(this)));
  websocket_->StartReceiving();

  state_ = State::kOpen;
  std::move(on_open_).Run(std::string(), std::string(),
                          response ? response->selected_protocol
                                   : std::string());
}

void NexusWebSocketConnection::OnDataFrame(
    bool fin,
    network::mojom::WebSocketMessageType type,
    uint64_t data_length) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ != State::kOpen) {
    return;
  }
  if (type != network::mojom::WebSocketMessageType::CONTINUATION) {
    // The first frame of a message names its type.
    message_binary_ = type == network::mojom::WebSocketMessageType::BINARY;
    message_.clear();
  }
  if (!discarding_ && (data_length > kMaxMessageBytes ||
                       message_.size() + data_length > kMaxMessageBytes)) {
    // The frame's bytes still follow on the pipe: drain them (and every
    // later frame's) rather than leave them to be read as the next frame.
    discarding_ = true;
    message_.clear();
    Close(1009, "message too big");
  }
  frame_fin_ = fin;
  frame_remaining_ = static_cast<size_t>(data_length);
  if (frame_remaining_ == 0) {
    if (fin && !discarding_) {
      FinishMessage();
    }
    return;
  }
  // No further frame until this one's bytes are off the pipe.
  client_receiver_.Pause();
  ReadFrame(MOJO_RESULT_OK, mojo::HandleSignalsState());
}

void NexusWebSocketConnection::ReadFrame(
    MojoResult /*result*/,
    const mojo::HandleSignalsState& /*state*/) {
  while (frame_remaining_ > 0) {
    size_t read = 0;
    MojoResult result;
    if (discarding_) {
      result = readable_->DiscardData(frame_remaining_, read);
    } else {
      const size_t offset = message_.size();
      message_.resize(offset + frame_remaining_);
      result = readable_->ReadData(
          MOJO_READ_DATA_FLAG_NONE,
          base::as_writable_byte_span(message_).subspan(offset), read);
      message_.resize(offset + (result == MOJO_RESULT_OK ? read : 0));
    }
    if (result == MOJO_RESULT_SHOULD_WAIT) {
      readable_watcher_.ArmOrNotify();
      return;
    }
    if (result != MOJO_RESULT_OK) {
      Drop(1006, "read failed", /*was_clean=*/false);
      return;
    }
    frame_remaining_ -= read;
  }
  client_receiver_.Resume();
  if (frame_fin_ && !discarding_) {
    FinishMessage();
  }
}

void NexusWebSocketConnection::FinishMessage() {
  std::string data;
  data.swap(message_);
  on_message_.Run(message_binary_, std::move(data));
}

bool NexusWebSocketConnection::Send(bool binary, std::string data) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ != State::kOpen) {
    return false;
  }
  // The network service reads exactly `size` bytes for this message from
  // the pipe, in SendMessage order — so queue order is wire order.
  websocket_->SendMessage(binary ? network::mojom::WebSocketMessageType::BINARY
                                 : network::mojom::WebSocketMessageType::TEXT,
                          data.size());
  if (!data.empty()) {
    write_queue_.push_back(std::move(data));
  }
  PumpWrites(MOJO_RESULT_OK, mojo::HandleSignalsState());
  return true;
}

void NexusWebSocketConnection::PumpWrites(
    MojoResult /*result*/,
    const mojo::HandleSignalsState& /*state*/) {
  while (!write_queue_.empty() && state_ == State::kOpen) {
    const std::string& front = write_queue_.front();
    size_t written = 0;
    const MojoResult result = writable_->WriteData(
        base::as_byte_span(front).subspan(write_offset_),
        MOJO_WRITE_DATA_FLAG_NONE, written);
    if (result == MOJO_RESULT_SHOULD_WAIT) {
      writable_watcher_.ArmOrNotify();
      return;
    }
    if (result != MOJO_RESULT_OK) {
      Drop(1006, "write failed", /*was_clean=*/false);
      return;
    }
    write_offset_ += written;
    if (write_offset_ == front.size()) {
      write_queue_.pop_front();
      write_offset_ = 0;
    }
  }
}

void NexusWebSocketConnection::Close(uint16_t code, const std::string& reason) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ == State::kOpen && websocket_.is_bound()) {
    // OnDropChannel reports the end.
    websocket_->StartClosingHandshake(code, reason);
    return;
  }
  if (state_ != State::kClosed) {
    FailOpen("CANCELLED", "The socket was closed before it opened.");
  }
}

void NexusWebSocketConnection::OnDropChannel(bool was_clean,
                                             uint16_t code,
                                             const std::string& reason) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  Drop(code, reason, was_clean);
}

void NexusWebSocketConnection::OnClosingHandshake() {
  // The network service answers the peer's close frame itself; the end
  // arrives as OnDropChannel.
}

void NexusWebSocketConnection::FailOpen(std::string code,
                                        std::string message) {
  if (state_ == State::kClosed) {
    return;
  }
  state_ = State::kClosed;
  proxy_receiver_.reset();
  handshake_receiver_.reset();
  if (on_open_) {
    std::move(on_open_).Run(std::move(code), std::move(message),
                            std::string());
  }
}

void NexusWebSocketConnection::Drop(uint16_t code,
                                    std::string reason,
                                    bool was_clean) {
  if (state_ == State::kClosed) {
    return;
  }
  const bool was_open = state_ == State::kOpen;
  state_ = State::kClosed;
  readable_watcher_.Cancel();
  writable_watcher_.Cancel();
  client_receiver_.reset();
  websocket_.reset();
  write_queue_.clear();
  if (!was_open) {
    if (on_open_) {
      std::move(on_open_).Run("WS_CONNECT_FAILED", std::move(reason),
                              std::string());
    }
    return;
  }
  if (on_closed_) {
    std::move(on_closed_).Run(code, std::move(reason), was_clean);
  }
}

void NexusWebSocketConnection::OnPipeDisconnected() {
  if (state_ == State::kOpen) {
    Drop(1006, "connection lost", /*was_clean=*/false);
    return;
  }
  FailOpen("WS_CONNECT_FAILED", "The network service dropped the handshake.");
}

}  // namespace nexus
