// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/mcp/nexus_mcp_stdio_transport.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/strcat.h"
#include "custom_browser/browser/nexus/tools/process_tool/nexus_shell_service_launcher.h"

namespace nexus {

namespace {
constexpr size_t kStderrTailCap = 4096;
}  // namespace

NexusMcpStdioTransport::NexusMcpStdioTransport(
    NexusMcpServerConfig config,
    NexusShellServiceLauncher* launcher,
    base::FilePath default_cwd)
    : config_(std::move(config)),
      launcher_(launcher),
      default_cwd_(std::move(default_cwd)) {}

NexusMcpStdioTransport::~NexusMcpStdioTransport() = default;

void NexusMcpStdioTransport::Start(base::OnceClosure on_ready,
                                   MessageCallback on_message,
                                   ClosedCallback on_closed) {
  on_ready_ = std::move(on_ready);
  on_message_ = std::move(on_message);
  on_closed_ = std::move(on_closed);
  started_ = true;

  if (!launcher_) {
    closed_ = true;
    on_closed_.Run("no shell-service launcher available");
    return;
  }

  auto options = mojom::ShellSpawnOptions::New();
  options->command = config_.command;
  options->args = config_.args;
  options->cwd = config_.cwd.empty() ? default_cwd_ : config_.cwd;
  for (const auto& [name, value] : config_.env) {
    auto entry = mojom::ShellEnvEntry::New();
    entry->name = name;
    entry->value = value;
    options->env.push_back(std::move(entry));
  }
  options->use_shell = false;
  options->stdin_data = std::string();
  options->timeout_ms = 0;
  options->max_output_bytes = 0;

  auto controller_receiver = controller_.BindNewPipeAndPassReceiver();
  auto client_remote = client_receiver_.BindNewPipeAndPassRemote();

  launcher_->GetService()->StreamSpawn(
      std::move(options), std::move(controller_receiver),
      std::move(client_remote),
      base::BindOnce(&NexusMcpStdioTransport::OnStreamSpawned,
                     weak_factory_.GetWeakPtr()));
}

void NexusMcpStdioTransport::OnStreamSpawned(
    bool ok,
    const std::optional<std::string>& error_code,
    const std::optional<std::string>& error_message,
    const std::optional<std::string>& install_hint) {
  if (!ok) {
    std::string msg = error_message.value_or("StreamSpawn failed");
    if (error_code) {
      msg = base::StrCat({*error_code, ": ", msg});
    }
    if (install_hint && !install_hint->empty()) {
      msg = base::StrCat({msg, "\n", *install_hint});
    }
    closed_ = true;
    if (on_closed_) {
      on_closed_.Run(msg);
    }
    return;
  }
  if (on_ready_) {
    std::move(on_ready_).Run();
  }
}

void NexusMcpStdioTransport::Send(const base::DictValue& message) {
  if (!controller_ || closed_) {
    return;
  }
  std::string json;
  if (!base::JSONWriter::Write(message, &json)) {
    LOG(WARNING) << "[mcp] Failed to serialize JSON-RPC message for \""
                 << config_.id << "\"";
    return;
  }
  json.push_back('\n');  // newline-delimited framing
  std::vector<uint8_t> bytes(json.begin(), json.end());
  controller_->WriteStdin(bytes);
}

void NexusMcpStdioTransport::Close() {
  closed_ = true;
  if (controller_) {
    controller_->Kill();
  }
  controller_.reset();
  client_receiver_.reset();
}

void NexusMcpStdioTransport::OnStdout(const std::vector<uint8_t>& data) {
  read_buffer_.append(reinterpret_cast<const char*>(data.data()), data.size());
  ProcessIncoming();
}

void NexusMcpStdioTransport::OnStderr(const std::vector<uint8_t>& data) {
  if (stderr_tail_.size() >= kStderrTailCap) {
    return;
  }
  const size_t room = kStderrTailCap - stderr_tail_.size();
  stderr_tail_.append(reinterpret_cast<const char*>(data.data()),
                      std::min(room, data.size()));
}

void NexusMcpStdioTransport::OnExit(mojom::ShellExitInfoPtr exit) {
  controller_.reset();
  if (closed_) {
    return;
  }
  closed_ = true;
  std::string msg = "server process exited";
  if (!stderr_tail_.empty()) {
    msg = base::StrCat({msg, " (stderr: ", stderr_tail_, ")"});
  }
  if (on_closed_) {
    on_closed_.Run(msg);
  }
}

void NexusMcpStdioTransport::ProcessIncoming() {
  size_t pos;
  while ((pos = read_buffer_.find('\n')) != std::string::npos) {
    std::string line = read_buffer_.substr(0, pos);
    read_buffer_.erase(0, pos + 1);
    if (line.empty()) {
      continue;
    }
    std::optional<base::Value> parsed =
        base::JSONReader::Read(line, base::JSON_PARSE_RFC);
    if (!parsed || !parsed->is_dict()) {
      LOG(WARNING) << "[mcp] Non-JSON line from \"" << config_.id << "\"";
      continue;
    }
    if (on_message_) {
      on_message_.Run(std::move(*parsed).TakeDict());
    }
  }
}

}  // namespace nexus
