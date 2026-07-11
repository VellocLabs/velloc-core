// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_MCP_NEXUS_MCP_HOST_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_MCP_NEXUS_MCP_HOST_H_

#include <map>
#include <memory>
#include <string>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "custom_browser/browser/nexus/api/mcp/nexus_mcp_server_store.h"
#include "custom_browser/browser/nexus/api/mcp/nexus_mcp_transport.h"

namespace network {
class SharedURLLoaderFactory;
}  // namespace network

namespace nexus {

class NexusShellServiceLauncher;

// Browser-side MCP client for ONE server. Owns the transport-agnostic
// JSON-RPC 2.0 machinery (request ids, pending map, the `initialize` →
// `notifications/initialized` → `tools/list` handshake, `tools/call`) and
// delegates the byte transport to a `NexusMcpTransport` chosen by
// `config.transport`:
//   * "stdio" — NexusMcpStdioTransport (slice-4 streaming child).
//   * "http"  — NexusMcpHttpTransport (Streamable HTTP POST).
//
// One instance per configured server, owned by `NexusMcpBridgeAdapter`.
// Lives on the bridge (UI) sequence.
//
// SECRETS: the server's command / args / env (stdio) or url / headers
// (http) come from prefs and stay in this process; only tool schemas +
// results flow back to the guest.
class NexusMcpHost {
 public:
  // ok + tools (a list of {name, description?, inputSchema?} dicts) on
  // success; ok=false + error otherwise. `tools` is empty on failure.
  using ConnectCallback =
      base::OnceCallback<void(bool ok,
                              base::ListValue tools,
                              std::string error)>;
  // ok + the raw MCP `tools/call` result object on success.
  using CallToolCallback =
      base::OnceCallback<void(bool ok, base::Value result, std::string error)>;

  // `launcher` (stdio) and `url_loader_factory` (http) outlive this host
  // (owned by the adapter). `default_cwd` is used when a stdio config
  // omits an explicit cwd.
  NexusMcpHost(
      NexusMcpServerConfig config,
      NexusShellServiceLauncher* launcher,
      base::FilePath default_cwd,
      scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory);

  NexusMcpHost(const NexusMcpHost&) = delete;
  NexusMcpHost& operator=(const NexusMcpHost&) = delete;

  ~NexusMcpHost();

  void Connect(ConnectCallback callback);
  void CallTool(const std::string& tool_name,
                base::DictValue arguments,
                CallToolCallback callback);
  void Disconnect();

  bool is_connected() const { return state_ == State::kConnected; }

  // Server identity from the `initialize` response's `serverInfo` (empty
  // until the handshake's initialize step completes). Used by the settings
  // connection test to auto-name a server the user left unnamed.
  const std::string& server_name() const { return server_name_; }
  const std::string& server_version() const { return server_version_; }

 private:
  enum class State { kIdle, kConnecting, kConnected, kFailed, kClosed };

  using JsonRpcResponseCallback =
      base::OnceCallback<void(bool ok, base::Value result, std::string error)>;

  // Build the transport for `config_.transport`. Returns null for an
  // unsupported transport.
  std::unique_ptr<NexusMcpTransport> MakeTransport();

  // Transport callbacks.
  void OnTransportReady();
  void OnTransportMessage(base::DictValue message);
  void OnTransportClosed(const std::string& error);

  // Handshake steps.
  void OnInitializeResult(bool ok, base::Value result, std::string error);
  void OnToolsListResult(bool ok, base::Value result, std::string error);

  // JSON-RPC plumbing.
  void SendRequest(const std::string& method,
                   base::DictValue params,
                   JsonRpcResponseCallback callback);
  void SendNotification(const std::string& method, base::DictValue params);

  // Failure paths.
  void FailConnect(const std::string& error);
  void FailAllPending(const std::string& error);
  void OnConnectTimeout();

  NexusMcpServerConfig config_;
  raw_ptr<NexusShellServiceLauncher> launcher_;
  base::FilePath default_cwd_;
  scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory_;

  State state_ = State::kIdle;
  std::unique_ptr<NexusMcpTransport> transport_;

  // JSON-RPC request ids. `int` round-trips through base::Value's 32-bit
  // int and matches on the echoed response; a session never approaches
  // 2^31 requests.
  int next_id_ = 1;
  std::map<int, JsonRpcResponseCallback> pending_;

  ConnectCallback connect_callback_;
  base::ListValue cached_tools_;
  // Parsed from the `initialize` response's `serverInfo` (best-effort).
  std::string server_name_;
  std::string server_version_;
  base::OneShotTimer connect_timer_;

  base::WeakPtrFactory<NexusMcpHost> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_MCP_NEXUS_MCP_HOST_H_
