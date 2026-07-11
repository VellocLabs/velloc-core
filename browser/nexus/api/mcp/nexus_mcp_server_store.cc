// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/mcp/nexus_mcp_server_store.h"

#include "base/logging.h"
#include "base/values.h"
#include "components/prefs/pref_service.h"
#include "custom_browser/browser/nexus/api/llm/llm_pref_names.h"

namespace nexus {

namespace {

// Default transport when a config omits the field. Phase 1 supports only
// stdio, so a missing transport is treated as stdio.
constexpr char kDefaultTransport[] = "stdio";

}  // namespace

NexusMcpServerConfig::NexusMcpServerConfig() = default;
NexusMcpServerConfig::~NexusMcpServerConfig() = default;
NexusMcpServerConfig::NexusMcpServerConfig(const NexusMcpServerConfig&) =
    default;
NexusMcpServerConfig& NexusMcpServerConfig::operator=(
    const NexusMcpServerConfig&) = default;
NexusMcpServerConfig::NexusMcpServerConfig(NexusMcpServerConfig&&) = default;
NexusMcpServerConfig& NexusMcpServerConfig::operator=(NexusMcpServerConfig&&) =
    default;

NexusMcpServerStore::NexusMcpServerStore(PrefService* prefs) : prefs_(prefs) {}

NexusMcpServerStore::~NexusMcpServerStore() = default;

std::vector<NexusMcpServerSummary> NexusMcpServerStore::ListSummaries() const {
  std::vector<NexusMcpServerSummary> out;
  if (!prefs_) {
    return out;
  }
  const base::DictValue& all = prefs_->GetDict(prefs::kNexusMcpServerConfigs);
  for (const auto [id, value] : all) {
    const base::DictValue* config = value.GetIfDict();
    if (!config) {
      LOG(WARNING) << "[mcp] Server config \"" << id
                   << "\" is not a dict; skipping.";
      continue;
    }
    NexusMcpServerSummary summary;
    summary.id = id;
    const std::string* transport = config->FindString("transport");
    summary.transport = transport ? *transport : kDefaultTransport;
    out.push_back(std::move(summary));
  }
  return out;
}

std::optional<NexusMcpServerConfig> NexusMcpServerStore::GetConfig(
    const std::string& id) const {
  if (!prefs_) {
    return std::nullopt;
  }
  const base::DictValue& all = prefs_->GetDict(prefs::kNexusMcpServerConfigs);
  const base::DictValue* config = all.FindDict(id);
  if (!config) {
    return std::nullopt;
  }
  return ParseConfig(id, *config);
}

// static
std::optional<NexusMcpServerConfig> NexusMcpServerStore::ParseConfig(
    const std::string& id,
    const base::DictValue& raw_config) {
  const base::DictValue* config = &raw_config;
  NexusMcpServerConfig out;
  out.id = id;
  const std::string* transport = config->FindString("transport");
  out.transport = transport ? *transport : kDefaultTransport;

  if (out.transport == "http") {
    const std::string* url = config->FindString("url");
    if (!url || url->empty()) {
      LOG(WARNING) << "[mcp] http server \"" << id << "\" missing url.";
      return std::nullopt;
    }
    out.url = *url;
    if (const base::DictValue* headers = config->FindDict("headers")) {
      for (const auto [name, val] : *headers) {
        if (const std::string* s = val.GetIfString()) {
          out.headers.emplace_back(name, *s);
        }
      }
    }
    return out;
  }

  // stdio (default).
  const std::string* command = config->FindString("command");
  if (!command || command->empty()) {
    LOG(WARNING) << "[mcp] stdio server \"" << id << "\" missing command.";
    return std::nullopt;
  }
  out.command = *command;

  if (const base::ListValue* args = config->FindList("args")) {
    for (const base::Value& arg : *args) {
      if (const std::string* s = arg.GetIfString()) {
        out.args.push_back(*s);
      }
    }
  }

  if (const base::DictValue* env = config->FindDict("env")) {
    for (const auto [name, val] : *env) {
      if (const std::string* s = val.GetIfString()) {
        out.env.emplace_back(name, *s);
      }
    }
  }

  if (const std::string* cwd = config->FindString("cwd");
      cwd && !cwd->empty()) {
    out.cwd = base::FilePath::FromUTF8Unsafe(*cwd);
  }

  return out;
}

}  // namespace nexus
