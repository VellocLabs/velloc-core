// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_MCP_NEXUS_MCP_SERVER_STORE_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_MCP_NEXUS_MCP_SERVER_STORE_H_

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"

class PrefService;

namespace base {
class DictValue;
}

namespace nexus {

// Public-facing summary of one configured native MCP server. Carries ONLY
// the opaque id + transport — the fields safe to hand the JS guest via
// `mcp.listServers`. Never includes command / args / env.
struct NexusMcpServerSummary {
  std::string id;
  std::string transport;  // "stdio" (phase 1)
};

// Full launch spec for one native MCP server, read browser-side only. The
// `env` entries may carry secrets (API tokens) — this struct must never be
// serialized to the guest.
struct NexusMcpServerConfig {
  NexusMcpServerConfig();
  ~NexusMcpServerConfig();
  NexusMcpServerConfig(const NexusMcpServerConfig&);
  NexusMcpServerConfig& operator=(const NexusMcpServerConfig&);
  NexusMcpServerConfig(NexusMcpServerConfig&&);
  NexusMcpServerConfig& operator=(NexusMcpServerConfig&&);

  std::string id;
  std::string transport;  // "stdio" | "http"

  // --- stdio fields ---
  std::string command;  // tool id (e.g. "node") or absolute path
  std::vector<std::string> args;
  // Ordered env overrides (name, value). Last duplicate wins, matching
  // ShellEnvEntry semantics. May carry secrets.
  std::vector<std::pair<std::string, std::string>> env;
  base::FilePath cwd;  // empty => caller supplies a default

  // --- http fields ---
  std::string url;  // absolute http(s) MCP endpoint
  // Request headers injected on every POST (e.g. Authorization). May
  // carry secrets.
  std::vector<std::pair<std::string, std::string>> headers;
};

// Reads `prefs::kNexusMcpServerConfigs`. Thin, stateless accessor (mirrors
// `GetStoredProviderConfig` for the LLM side) — constructed per use with a
// borrowed `PrefService*`.
class NexusMcpServerStore {
 public:
  explicit NexusMcpServerStore(PrefService* prefs);
  ~NexusMcpServerStore();

  NexusMcpServerStore(const NexusMcpServerStore&) = delete;
  NexusMcpServerStore& operator=(const NexusMcpServerStore&) = delete;

  // All configured servers, as id+transport summaries. Skips malformed
  // entries (logs a warning). Order follows the prefs dict iteration order.
  std::vector<NexusMcpServerSummary> ListSummaries() const;

  // Full config for one server, or nullopt if `id` is unknown / malformed.
  std::optional<NexusMcpServerConfig> GetConfig(const std::string& id) const;

  // Parse a raw server-config dict (transport + command/args/env or
  // url/headers) into a launch spec, independent of prefs. Returns nullopt
  // when required fields are missing (url for http, command for stdio).
  // Used both by `GetConfig` (stored configs) and by the settings handler's
  // pre-save connection test (an unsaved form config). The dict values are
  // taken verbatim — callers that read from prefs must decrypt secrets after
  // parsing; callers passing plaintext form data use them as-is.
  static std::optional<NexusMcpServerConfig> ParseConfig(
      const std::string& id,
      const base::DictValue& config);

 private:
  raw_ptr<PrefService> prefs_;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_MCP_NEXUS_MCP_SERVER_STORE_H_
