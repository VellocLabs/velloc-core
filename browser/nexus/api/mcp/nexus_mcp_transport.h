// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_MCP_NEXUS_MCP_TRANSPORT_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_MCP_NEXUS_MCP_TRANSPORT_H_

#include <string>

#include "base/functional/callback.h"
#include "base/values.h"

namespace nexus {

// Transport seam for `NexusMcpHost`. The host owns the transport-agnostic
// JSON-RPC machinery (request ids, pending map, `initialize` handshake);
// the transport owns *how* newline-delimited JSON-RPC messages get to and
// from the server. Two implementations:
//   * NexusMcpStdioTransport — slice-4 streaming child process (stdio).
//   * NexusMcpHttpTransport  — MCP Streamable HTTP (POST per message).
//
// All callbacks fire on the bridge (UI) sequence. The host guarantees it
// outlives the transport (it owns it via unique_ptr).
class NexusMcpTransport {
 public:
  // Delivers one parsed inbound JSON-RPC message (a response or a
  // server-initiated request/notification).
  using MessageCallback = base::RepeatingCallback<void(base::DictValue)>;
  // Terminal failure / close. Fires at most once meaningfully; the host
  // guards against acting on it twice. `error` is empty on a clean close.
  using ClosedCallback =
      base::RepeatingCallback<void(const std::string& error)>;

  virtual ~NexusMcpTransport() = default;

  // Establish the transport. `on_ready` fires once the transport can
  // accept `Send` (stdio: after the child spawns; http: ~immediately).
  // If the transport fails to start, it fires `on_closed` instead of
  // `on_ready`. `on_message` / `on_closed` may fire any time after.
  virtual void Start(base::OnceClosure on_ready,
                     MessageCallback on_message,
                     ClosedCallback on_closed) = 0;

  // Send one JSON-RPC message (a complete dict). Ordering is preserved.
  virtual void Send(const base::DictValue& message) = 0;

  // Tear down (kill the child / cancel in-flight requests). Idempotent.
  virtual void Close() = 0;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_MCP_NEXUS_MCP_TRANSPORT_H_
