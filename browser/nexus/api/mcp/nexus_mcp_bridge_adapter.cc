// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/mcp/nexus_mcp_bridge_adapter.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/storage_partition.h"
#include "custom_browser/browser/nexus/api/mcp/nexus_mcp_host.h"
#include "custom_browser/browser/nexus/api/mcp/nexus_mcp_server_store.h"
#include "custom_browser/browser/nexus/nexus_secret_crypt.h"
#include "custom_browser/browser/nexus/tools/process_tool/nexus_shell_service_launcher.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"

namespace nexus {

NexusMcpBridgeAdapter::NexusMcpBridgeAdapter(
    NexusBridgeResponseDelegate* delegate,
    Profile* profile,
    base::FilePath default_cwd)
    : ::nexus::generated::McpApiBase(delegate),
      prefs_(profile ? profile->GetPrefs() : nullptr),
      default_cwd_(std::move(default_cwd)),
      launcher_(std::make_unique<NexusShellServiceLauncher>()) {
  if (profile) {
    url_loader_factory_ = profile->GetDefaultStoragePartition()
                              ->GetURLLoaderFactoryForBrowserProcess();
  }
  // Prime the at-rest secret encryptor early so it's ready by the time the
  // user connects a server (env / Authorization values are stored
  // encrypted; see GetOrCreateHost for the decrypt).
  NexusSecretCrypt::GetInstance()->EnsureAcquired(
      g_browser_process ? g_browser_process->os_crypt_async() : nullptr);
}

NexusMcpBridgeAdapter::~NexusMcpBridgeAdapter() = default;

void NexusMcpBridgeAdapter::HandleListServersOnUI(
    ::nexus::generated::ListServersInput /*input*/,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  NexusMcpServerStore store(prefs_);
  base::ListValue servers;
  for (const NexusMcpServerSummary& summary : store.ListSummaries()) {
    base::DictValue entry;
    entry.Set("id", summary.id);
    entry.Set("transport", summary.transport);
    servers.Append(std::move(entry));
  }
  base::DictValue result;
  result.Set("servers", std::move(servers));
  std::move(done).Success(std::move(result));
}

void NexusMcpBridgeAdapter::HandleConnectOnUI(
    ::nexus::generated::ConnectInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  // Two paths for resolving the host: inline config (post-migration —
  // JS pool reads `~/.nexus/settings.json` and passes the full config
  // through) or pref lookup (legacy fallback, still wired during
  // the migration window). Each path lands on the same NexusMcpHost
  // shape; only the source-of-truth differs.
  NexusMcpHost* host = input.config.has_value()
                           ? GetOrCreateHostFromConfig(input.serverId,
                                                       *input.config)
                           : GetOrCreateHost(input.serverId);
  if (!host) {
    base::DictValue result;
    result.Set("ok", false);
    result.Set("tools", base::ListValue());
    result.Set("error", "Unknown MCP server: " + input.serverId);
    std::move(done).Success(std::move(result));
    return;
  }
  host->Connect(base::BindOnce(&NexusMcpBridgeAdapter::OnHostConnected,
                               weak_factory_.GetWeakPtr(), std::move(done)));
}

void NexusMcpBridgeAdapter::HandleCallToolOnUI(
    ::nexus::generated::CallToolInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  auto it = hosts_.find(input.serverId);
  if (it == hosts_.end() || !it->second->is_connected()) {
    base::DictValue result;
    result.Set("ok", false);
    result.Set("error", "MCP server is not connected: " + input.serverId);
    std::move(done).Success(std::move(result));
    return;
  }
  it->second->CallTool(
      input.toolName, std::move(input.arguments),
      base::BindOnce(&NexusMcpBridgeAdapter::OnHostCalled,
                     weak_factory_.GetWeakPtr(), std::move(done)));
}

void NexusMcpBridgeAdapter::HandleDisconnectOnUI(
    ::nexus::generated::DisconnectInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  auto it = hosts_.find(input.serverId);
  if (it != hosts_.end()) {
    it->second->Disconnect();
    hosts_.erase(it);
  }
  base::DictValue result;
  result.Set("ok", true);
  std::move(done).Success(std::move(result));
}

NexusMcpHost* NexusMcpBridgeAdapter::GetOrCreateHostFromConfig(
    const std::string& id,
    const base::DictValue& config_dict) {
  auto it = hosts_.find(id);
  if (it != hosts_.end()) {
    return it->second.get();
  }
  // Parse the inline dict into the launch spec. ParseConfig is the
  // same path the legacy `GetOrCreateHost` uses (via `GetConfig`),
  // just sourced from the JS-supplied dict instead of from prefs.
  // Secrets arrive in plaintext from the JS side (settings.json holds
  // them as-is — different threat model from the encrypted-pref
  // path); no `NexusSecretCrypt::Decrypt` here.
  std::optional<NexusMcpServerConfig> config =
      NexusMcpServerStore::ParseConfig(id, config_dict);
  if (!config) {
    return nullptr;
  }
  auto host = std::make_unique<NexusMcpHost>(
      std::move(*config), launcher_.get(), default_cwd_, url_loader_factory_);
  NexusMcpHost* ptr = host.get();
  hosts_[id] = std::move(host);
  return ptr;
}

NexusMcpHost* NexusMcpBridgeAdapter::GetOrCreateHost(const std::string& id) {
  auto it = hosts_.find(id);
  if (it != hosts_.end()) {
    return it->second.get();
  }
  NexusMcpServerStore store(prefs_);
  std::optional<NexusMcpServerConfig> config = store.GetConfig(id);
  if (!config) {
    return nullptr;
  }
  // Secrets (stdio `env` values + http `Authorization`-style headers) are
  // stored encrypted at rest. Decrypt them here, browser-side, before they
  // reach the host / the spawned child. Unencrypted (legacy plaintext)
  // values pass through unchanged.
  NexusSecretCrypt* crypt = NexusSecretCrypt::GetInstance();
  for (auto& [name, value] : config->env) {
    value = crypt->Decrypt(value);
  }
  for (auto& [name, value] : config->headers) {
    value = crypt->Decrypt(value);
  }
  auto host = std::make_unique<NexusMcpHost>(
      std::move(*config), launcher_.get(), default_cwd_, url_loader_factory_);
  NexusMcpHost* ptr = host.get();
  hosts_[id] = std::move(host);
  return ptr;
}

void NexusMcpBridgeAdapter::OnHostConnected(::nexus::BridgeResponseCallback done,
                                            bool ok,
                                            base::ListValue tools,
                                            std::string error) {
  base::DictValue result;
  result.Set("ok", ok);
  result.Set("tools", std::move(tools));
  if (!ok) {
    result.Set("error", error);
  }
  std::move(done).Success(std::move(result));
}

void NexusMcpBridgeAdapter::OnHostCalled(::nexus::BridgeResponseCallback done,
                                         bool ok,
                                         base::Value result,
                                         std::string error) {
  base::DictValue out;
  out.Set("ok", ok);
  if (ok) {
    out.Set("result", std::move(result));
  } else {
    out.Set("error", error);
  }
  std::move(done).Success(std::move(out));
}

void NexusMcpBridgeAdapter::HandleTestConnectOnUI(
    ::nexus::generated::TestConnectInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  // The legacy `HandleTestMcpServer` sent PLAINTEXT secrets in the
  // request payload — the test runs before `upsertMcpServer` encrypts
  // them. Parse + connect as-is.
  // The id is optional on the wire — the form tests unnamed servers and
  // reads the server-declared name from the result. ParseConfig requires
  // a string; pass through unwrapped, defaulting to empty.
  const std::string id_str = input.id.value_or(std::string());
  std::optional<NexusMcpServerConfig> config =
      NexusMcpServerStore::ParseConfig(id_str, input.config);
  if (!config) {
    base::DictValue result;
    // Same `ok` contract as OnTestConnectComplete (this is testConnect's sync
    // early-return). The previous `success` key happened to read as a failure
    // (undefined → falsy) but was off-contract.
    result.Set("ok", false);
    result.Set("error",
               "Invalid server config. Check the transport and its required "
               "fields (URL for http, command for stdio).");
    std::move(done).Success(base::Value(std::move(result)));
    return;
  }

  // Transient host — never registered with `hosts_` or prefs. The
  // bridge adapter's existing shared `launcher_` is reused so we don't
  // spawn a second utility process. `default_cwd_` is the install-root
  // working directory used for stdio (http transport ignores it).
  auto host = std::make_unique<NexusMcpHost>(
      std::move(*config), launcher_.get(), default_cwd_, url_loader_factory_);
  NexusMcpHost* host_ptr = host.get();
  const size_t token = next_test_token_++;
  pending_test_hosts_[token] = std::move(host);
  host_ptr->Connect(
      base::BindOnce(&NexusMcpBridgeAdapter::OnTestConnectComplete,
                     weak_factory_.GetWeakPtr(), std::move(done), token));
}

void NexusMcpBridgeAdapter::OnTestConnectComplete(
    ::nexus::BridgeResponseCallback done,
    size_t token,
    bool ok,
    base::ListValue tools,
    std::string error) {
  std::string server_name;
  std::string server_version;
  auto it = pending_test_hosts_.find(token);
  if (it != pending_test_hosts_.end()) {
    server_name = it->second->server_name();
    server_version = it->second->server_version();
    // We're invoked synchronously from inside the host's own connect /
    // transport callback stack (NexusMcpHost::FailConnect /
    // OnToolsListResult run this callback, then keep touching host
    // members as the stack unwinds — e.g. FailConnect → FailAllPending).
    // Destroying the host here would free it out from under that
    // unwinding stack: use-after-free. Hand ownership to DeleteSoon so
    // the host (and its transport / child process, which its destructor
    // tears down) is freed only after the stack fully unwinds.
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
        FROM_HERE, std::move(it->second));
    pending_test_hosts_.erase(it);
  }

  base::DictValue result;
  // The wire contract (mcp.testConnect result, protocol/mcp.ts) is `ok`, and
  // the React consumer (McpSection) + every other handler in this file use
  // `ok`. Emitting `success` here left `test.ok` undefined on the TS side, so
  // even a successful probe read as a failure and aborted "Save & connect".
  result.Set("ok", ok);
  if (ok) {
    result.Set("toolCount", static_cast<int>(tools.size()));
    result.Set("tools", std::move(tools));
    if (!server_name.empty()) {
      result.Set("serverName", server_name);
    }
    if (!server_version.empty()) {
      result.Set("serverVersion", server_version);
    }
  } else {
    result.Set("error", error.empty() ? "Connection failed." : error);
  }
  std::move(done).Success(base::Value(std::move(result)));
}

}  // namespace nexus
