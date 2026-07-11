// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/mcp/nexus_mcp_host.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/strcat.h"
#include "base/time/time.h"
#include "custom_browser/browser/nexus/api/mcp/nexus_mcp_http_transport.h"
#include "custom_browser/browser/nexus/api/mcp/nexus_mcp_stdio_transport.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"

namespace nexus {

namespace {

constexpr char kProtocolVersion[] = "2025-06-18";
constexpr char kClientName[] = "nexus-web";
constexpr char kClientVersion[] = "0.1.0";

// Bound on the handshake (initialize + tools/list). Past this the connect
// is failed so pool init can proceed.
constexpr base::TimeDelta kConnectTimeout = base::Seconds(30);

}  // namespace

NexusMcpHost::NexusMcpHost(
    NexusMcpServerConfig config,
    NexusShellServiceLauncher* launcher,
    base::FilePath default_cwd,
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory)
    : config_(std::move(config)),
      launcher_(launcher),
      default_cwd_(std::move(default_cwd)),
      url_loader_factory_(std::move(url_loader_factory)) {}

NexusMcpHost::~NexusMcpHost() = default;

std::unique_ptr<NexusMcpTransport> NexusMcpHost::MakeTransport() {
  if (config_.transport == "http") {
    return std::make_unique<NexusMcpHttpTransport>(config_, url_loader_factory_);
  }
  if (config_.transport == "stdio") {
    return std::make_unique<NexusMcpStdioTransport>(config_, launcher_,
                                                    default_cwd_);
  }
  return nullptr;
}

void NexusMcpHost::Connect(ConnectCallback callback) {
  if (state_ == State::kConnected) {
    std::move(callback).Run(/*ok=*/true, cached_tools_.Clone(), std::string());
    return;
  }
  if (state_ == State::kConnecting) {
    std::move(callback).Run(/*ok=*/false, base::ListValue(),
                            "connect already in progress");
    return;
  }

  transport_ = MakeTransport();
  if (!transport_) {
    std::move(callback).Run(
        /*ok=*/false, base::ListValue(),
        base::StrCat({"unsupported MCP transport: ", config_.transport}));
    return;
  }

  connect_callback_ = std::move(callback);
  state_ = State::kConnecting;

  transport_->Start(
      base::BindOnce(&NexusMcpHost::OnTransportReady,
                     weak_factory_.GetWeakPtr()),
      base::BindRepeating(&NexusMcpHost::OnTransportMessage,
                          weak_factory_.GetWeakPtr()),
      base::BindRepeating(&NexusMcpHost::OnTransportClosed,
                          weak_factory_.GetWeakPtr()));

  connect_timer_.Start(FROM_HERE, kConnectTimeout,
                       base::BindOnce(&NexusMcpHost::OnConnectTimeout,
                                      weak_factory_.GetWeakPtr()));
}

void NexusMcpHost::OnTransportReady() {
  // Handshake step 1: initialize.
  base::DictValue params;
  params.Set("protocolVersion", kProtocolVersion);
  params.Set("capabilities", base::DictValue());
  base::DictValue client_info;
  client_info.Set("name", kClientName);
  client_info.Set("version", kClientVersion);
  params.Set("clientInfo", std::move(client_info));
  SendRequest("initialize", std::move(params),
              base::BindOnce(&NexusMcpHost::OnInitializeResult,
                             weak_factory_.GetWeakPtr()));
}

void NexusMcpHost::OnInitializeResult(bool ok,
                                      base::Value result,
                                      std::string error) {
  if (!ok) {
    FailConnect(base::StrCat({"initialize failed: ", error}));
    return;
  }
  // Capture server identity (best-effort) so the settings connection test
  // can auto-name an unnamed server. `{ serverInfo: { name, version } }`.
  if (const base::DictValue* dict = result.GetIfDict()) {
    if (const base::DictValue* info = dict->FindDict("serverInfo")) {
      if (const std::string* name = info->FindString("name")) {
        server_name_ = *name;
      }
      if (const std::string* version = info->FindString("version")) {
        server_version_ = *version;
      }
    }
  }
  SendNotification("notifications/initialized", base::DictValue());
  SendRequest("tools/list", base::DictValue(),
              base::BindOnce(&NexusMcpHost::OnToolsListResult,
                             weak_factory_.GetWeakPtr()));
}

void NexusMcpHost::OnToolsListResult(bool ok,
                                     base::Value result,
                                     std::string error) {
  if (!ok) {
    FailConnect(base::StrCat({"tools/list failed: ", error}));
    return;
  }

  base::ListValue tools;
  if (const base::DictValue* dict = result.GetIfDict()) {
    if (const base::ListValue* list = dict->FindList("tools")) {
      for (const base::Value& entry : *list) {
        const base::DictValue* td = entry.GetIfDict();
        if (!td) {
          continue;
        }
        base::DictValue out;
        if (const std::string* name = td->FindString("name")) {
          out.Set("name", *name);
        } else {
          continue;
        }
        if (const std::string* desc = td->FindString("description")) {
          out.Set("description", *desc);
        }
        if (const base::Value* schema = td->Find("inputSchema")) {
          out.Set("inputSchema", schema->Clone());
        }
        tools.Append(std::move(out));
      }
    }
  }

  cached_tools_ = tools.Clone();
  state_ = State::kConnected;
  connect_timer_.Stop();
  if (connect_callback_) {
    std::move(connect_callback_).Run(/*ok=*/true, std::move(tools),
                                     std::string());
  }
}

void NexusMcpHost::CallTool(const std::string& tool_name,
                            base::DictValue arguments,
                            CallToolCallback callback) {
  if (state_ != State::kConnected) {
    std::move(callback).Run(/*ok=*/false, base::Value(),
                            "MCP server is not connected");
    return;
  }
  base::DictValue params;
  params.Set("name", tool_name);
  params.Set("arguments", std::move(arguments));
  // CallToolCallback and JsonRpcResponseCallback share a signature.
  SendRequest("tools/call", std::move(params), std::move(callback));
}

void NexusMcpHost::Disconnect() {
  if (transport_) {
    transport_->Close();
  }
  connect_timer_.Stop();
  state_ = State::kClosed;
  FailAllPending("MCP server disconnected");
}

void NexusMcpHost::SendRequest(const std::string& method,
                               base::DictValue params,
                               JsonRpcResponseCallback callback) {
  const int id = next_id_++;
  pending_[id] = std::move(callback);
  base::DictValue message;
  message.Set("jsonrpc", "2.0");
  message.Set("id", id);
  message.Set("method", method);
  message.Set("params", std::move(params));
  if (transport_) {
    transport_->Send(message);
  }
}

void NexusMcpHost::SendNotification(const std::string& method,
                                    base::DictValue params) {
  base::DictValue message;
  message.Set("jsonrpc", "2.0");
  message.Set("method", method);
  message.Set("params", std::move(params));
  if (transport_) {
    transport_->Send(message);
  }
}

void NexusMcpHost::OnTransportMessage(base::DictValue message) {
  const std::optional<int> id = message.FindInt("id");
  const bool has_method = message.contains("method");

  // id + no method => a RESPONSE to one of our requests.
  if (id.has_value() && !has_method) {
    auto it = pending_.find(*id);
    if (it == pending_.end()) {
      return;
    }
    JsonRpcResponseCallback callback = std::move(it->second);
    pending_.erase(it);

    if (const base::DictValue* err = message.FindDict("error")) {
      const std::string* m = err->FindString("message");
      std::move(callback).Run(/*ok=*/false, base::Value(),
                              m ? *m : "JSON-RPC error");
      return;
    }
    base::Value* result = message.Find("result");
    std::move(callback).Run(/*ok=*/true,
                            result ? result->Clone() : base::Value(),
                            std::string());
    return;
  }

  // Server-initiated request/notification — Phase 1/2 implements no server
  // capabilities; log and drop.
  if (has_method) {
    const std::string* method = message.FindString("method");
    VLOG(1) << "[mcp] Ignoring server-initiated \""
            << (method ? *method : "?") << "\" from \"" << config_.id << "\"";
  }
}

void NexusMcpHost::OnTransportClosed(const std::string& error) {
  const bool was_connecting = state_ == State::kConnecting;
  connect_timer_.Stop();
  if (was_connecting) {
    FailConnect(error);
    return;
  }
  if (state_ != State::kClosed) {
    state_ = State::kClosed;
  }
  FailAllPending(error.empty() ? "server connection closed" : error);
}

void NexusMcpHost::FailConnect(const std::string& error) {
  state_ = State::kFailed;
  connect_timer_.Stop();
  if (connect_callback_) {
    std::move(connect_callback_).Run(/*ok=*/false, base::ListValue(), error);
  }
  FailAllPending(error);
}

void NexusMcpHost::FailAllPending(const std::string& error) {
  std::map<int, JsonRpcResponseCallback> pending;
  pending.swap(pending_);
  for (auto& [id, callback] : pending) {
    std::move(callback).Run(/*ok=*/false, base::Value(), error);
  }
}

void NexusMcpHost::OnConnectTimeout() {
  FailConnect("timed out waiting for initialize / tools/list");
}

}  // namespace nexus
