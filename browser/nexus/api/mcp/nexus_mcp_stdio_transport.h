// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_MCP_NEXUS_MCP_STDIO_TRANSPORT_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_MCP_NEXUS_MCP_STDIO_TRANSPORT_H_

#include <cstdint>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "custom_browser/browser/nexus/api/mcp/nexus_mcp_server_store.h"
#include "custom_browser/browser/nexus/api/mcp/nexus_mcp_transport.h"
#include "custom_browser/browser/nexus/mojom/nexus_shell.mojom.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"

namespace nexus {

class NexusShellServiceLauncher;

// stdio MCP transport: launches the server as a long-lived child via the
// slice-4 streaming primitive and frames JSON-RPC as newline-delimited
// JSON over its stdin/stdout. The command / args / env (incl. secrets)
// come from the native config and never cross to the guest.
class NexusMcpStdioTransport : public NexusMcpTransport,
                               public mojom::NexusStreamingProcessClient {
 public:
  NexusMcpStdioTransport(NexusMcpServerConfig config,
                         NexusShellServiceLauncher* launcher,
                         base::FilePath default_cwd);

  NexusMcpStdioTransport(const NexusMcpStdioTransport&) = delete;
  NexusMcpStdioTransport& operator=(const NexusMcpStdioTransport&) = delete;

  ~NexusMcpStdioTransport() override;

  // NexusMcpTransport:
  void Start(base::OnceClosure on_ready,
             MessageCallback on_message,
             ClosedCallback on_closed) override;
  void Send(const base::DictValue& message) override;
  void Close() override;

 private:
  // mojom::NexusStreamingProcessClient:
  void OnStdout(const std::vector<uint8_t>& data) override;
  void OnStderr(const std::vector<uint8_t>& data) override;
  void OnExit(mojom::ShellExitInfoPtr exit) override;

  void OnStreamSpawned(bool ok,
                       const std::optional<std::string>& error_code,
                       const std::optional<std::string>& error_message,
                       const std::optional<std::string>& install_hint);
  // Splits `read_buffer_` into newline-delimited frames + dispatches.
  void ProcessIncoming();

  NexusMcpServerConfig config_;
  raw_ptr<NexusShellServiceLauncher> launcher_;
  base::FilePath default_cwd_;

  mojo::Remote<mojom::NexusStreamingProcess> controller_;
  mojo::Receiver<mojom::NexusStreamingProcessClient> client_receiver_{this};

  base::OnceClosure on_ready_;
  MessageCallback on_message_;
  ClosedCallback on_closed_;

  std::string read_buffer_;
  // Capped stderr tail, surfaced in the close error if the child dies
  // before completing the handshake.
  std::string stderr_tail_;
  bool started_ = false;
  bool closed_ = false;

  base::WeakPtrFactory<NexusMcpStdioTransport> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_MCP_NEXUS_MCP_STDIO_TRANSPORT_H_
