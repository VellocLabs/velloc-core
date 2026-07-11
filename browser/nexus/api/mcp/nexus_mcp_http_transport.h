// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_MCP_NEXUS_MCP_HTTP_TRANSPORT_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_MCP_NEXUS_MCP_HTTP_TRANSPORT_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "custom_browser/browser/nexus/api/mcp/nexus_mcp_server_store.h"
#include "custom_browser/browser/nexus/api/mcp/nexus_mcp_transport.h"
#include "url/gurl.h"

namespace network {
class SharedURLLoaderFactory;
class SimpleURLLoader;
}  // namespace network

namespace nexus {

// MCP Streamable HTTP transport (Phase 2). Each outbound JSON-RPC message
// is POSTed to the server URL via `network::SimpleURLLoader` (browser
// process → NOT subject to renderer CORS, same egress as the LLM
// providers). Secret headers (e.g. `Authorization`) come from the native
// config and are injected here — never seen by the guest.
//
// Response handling: the POST reply is parsed by content type —
// `application/json` (one message) or `text/event-stream` (SSE; each
// `data:` payload is one message). The `Mcp-Session-Id` returned by
// `initialize` is captured and echoed on later requests.
//
// Phase-2 scope: the OPTIONAL long-lived GET SSE channel for
// server-initiated messages is not opened (tool-calling only needs
// POST request/response). Documented in nexus-web-parity-mcp.md.
class NexusMcpHttpTransport : public NexusMcpTransport {
 public:
  NexusMcpHttpTransport(
      NexusMcpServerConfig config,
      scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory);

  NexusMcpHttpTransport(const NexusMcpHttpTransport&) = delete;
  NexusMcpHttpTransport& operator=(const NexusMcpHttpTransport&) = delete;

  ~NexusMcpHttpTransport() override;

  // NexusMcpTransport:
  void Start(base::OnceClosure on_ready,
             MessageCallback on_message,
             ClosedCallback on_closed) override;
  void Send(const base::DictValue& message) override;
  void Close() override;

 private:
  void OnResponse(network::SimpleURLLoader* loader,
                  std::optional<std::string> body);
  // Parse an SSE body, dispatching each `data:` JSON payload as a message.
  void DispatchSse(const std::string& body);
  // Parse + dispatch one JSON object string.
  void DispatchJson(const std::string& text);
  void EraseLoader(network::SimpleURLLoader* loader);

  NexusMcpServerConfig config_;
  GURL url_;
  scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory_;

  MessageCallback on_message_;
  ClosedCallback on_closed_;

  // Set from the `initialize` response; echoed as `Mcp-Session-Id` on
  // subsequent requests when non-empty.
  std::string session_id_;
  bool closed_ = false;

  std::vector<std::unique_ptr<network::SimpleURLLoader>> in_flight_;

  base::WeakPtrFactory<NexusMcpHttpTransport> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_MCP_NEXUS_MCP_HTTP_TRANSPORT_H_
