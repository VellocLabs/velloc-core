// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_MCP_NEXUS_MCP_BRIDGE_ADAPTER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_MCP_NEXUS_MCP_BRIDGE_ADAPTER_H_

#include <map>
#include <memory>
#include <string>

#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "custom_browser/browser/nexus/api/mcp/mcp_api.gen.h"
#include "custom_browser/browser/nexus/bridge_response_callback.h"

class PrefService;
class Profile;

namespace network {
class SharedURLLoaderFactory;
}  // namespace network

namespace nexus {

class NexusBridgeResponseDelegate;
class NexusMcpHost;
class NexusShellServiceLauncher;

// Concrete adapter implementing the generated `McpApiBase` virtuals — the
// `mcp` bridge capability (native-backed stdio MCP servers).
//
// Threading: every Handle*OnUI runs on the UI (bridge) thread. The actual
// MCP work is async — `connect` / `callTool` post into a per-server
// `NexusMcpHost` and complete via a `BridgeResponseCallback` captured in a
// bound continuation. Connection / tool failures are reported IN-BAND
// (`done.Success` with `ok:false` + `error`), never `done.Error`, matching
// the JS contract in `protocol/mcp.ts`.
//
// SECRETS: the adapter reads each server's command / args / env from
// `prefs::kNexusMcpServerConfigs` (via `NexusMcpServerStore`) and hands
// them to the host, which launches the server natively. Only ids,
// transports, tool schemas, and tool results ever cross back to the guest.
//
// Lifetime: owned by `NexusBridgePortService`; outlives every
// `NexusMcpHost` it owns. The shared `NexusShellServiceLauncher` is owned
// here (one streaming-capable utility process for all this bridge's MCP
// servers).
class NexusMcpBridgeAdapter : public ::nexus::generated::McpApiBase {
 public:
  NexusMcpBridgeAdapter(NexusBridgeResponseDelegate* delegate,
                        Profile* profile,
                        base::FilePath default_cwd);
  ~NexusMcpBridgeAdapter() override;

  NexusMcpBridgeAdapter(const NexusMcpBridgeAdapter&) = delete;
  NexusMcpBridgeAdapter& operator=(const NexusMcpBridgeAdapter&) = delete;

 private:
  // generated::McpApiBase:
  void HandleListServersOnUI(
      ::nexus::generated::ListServersInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleConnectOnUI(
      ::nexus::generated::ConnectInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleCallToolOnUI(
      ::nexus::generated::CallToolInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleDisconnectOnUI(
      ::nexus::generated::DisconnectInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  // Tier 3a (A5): one-shot connection probe. Spawns a TRANSIENT
  // NexusMcpHost from the inline config, runs initialize + tools/list,
  // returns `{success, tools, toolCount, serverName?, serverVersion?,
  // error?}`, and tears the host down. Never persists. Mirrors the
  // legacy `NexusSettingsHandler::HandleTestMcpServer` flow — same
  // SequencedTaskRunner::DeleteSoon dance to avoid use-after-free
  // when the connect callback fires inside the host's own stack.
  void HandleTestConnectOnUI(
      ::nexus::generated::TestConnectInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  // testConnect completion. Reads serverName/version from the host
  // before scheduling its teardown, builds the in-band response, and
  // fires `done.Success`. `token` keys into `pending_test_hosts_`.
  void OnTestConnectComplete(::nexus::BridgeResponseCallback done,
                             size_t token,
                             bool ok,
                             base::ListValue tools,
                             std::string error);

  // Returns the existing host for `id`, or lazily creates one from the
  // store. Returns nullptr when `id` is not a configured native server.
  NexusMcpHost* GetOrCreateHost(const std::string& id);

  // Returns the existing host for `id`, or lazily creates one from the
  // inline config dict (parsed via NexusMcpServerStore::ParseConfig).
  // Used by the post-migration JS pool: it reads `~/.nexus/settings.json`
  // and passes the full server config through the `connect` bridge
  // payload instead of relying on a server-side pref lookup. Returns
  // nullptr if the dict isn't a valid launch spec.
  NexusMcpHost* GetOrCreateHostFromConfig(const std::string& id,
                                          const base::DictValue& config);

  // Host completion trampolines → in-band bridge responses.
  void OnHostConnected(::nexus::BridgeResponseCallback done,
                       bool ok,
                       base::ListValue tools,
                       std::string error);
  void OnHostCalled(::nexus::BridgeResponseCallback done,
                    bool ok,
                    base::Value result,
                    std::string error);

  raw_ptr<PrefService> prefs_;
  base::FilePath default_cwd_;
  std::unique_ptr<NexusShellServiceLauncher> launcher_;
  // Acquired once at construction from the profile's default storage
  // partition (same factory the LLM providers + WebFetch use); handed to
  // each http-transport host. Null when no profile.
  scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory_;
  std::map<std::string, std::unique_ptr<NexusMcpHost>> hosts_;

  // Transient hosts in flight for `testConnect` probes. Keyed by a
  // monotonic token (an incrementing counter; the bridge's request id is
  // a string but we don't need to surface that to the host). Cleared in
  // `OnTestConnectComplete` via `DeleteSoon` to keep the host alive
  // through its own callback unwind, matching the legacy handler's
  // safety pattern.
  std::map<size_t, std::unique_ptr<NexusMcpHost>> pending_test_hosts_;
  size_t next_test_token_ = 0;

  base::WeakPtrFactory<NexusMcpBridgeAdapter> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_MCP_NEXUS_MCP_BRIDGE_ADAPTER_H_
