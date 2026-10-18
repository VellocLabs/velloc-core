// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/shell/nexus_shell_bridge_adapter.h"

#include <cinttypes>
#include <stdint.h>

#include <optional>
#include <string>
#include <utility>

#include "base/base64.h"
#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/profiles/profile.h"
#include "custom_browser/browser/nexus/api/secrets/nexus_secret_store.h"
#include "custom_browser/browser/nexus/api/shell/nexus_persistent_shells.h"
#include "custom_browser/browser/nexus/mojom/nexus_shell.mojom.h"
#include "custom_browser/browser/nexus/nexus_bridge_support.h"
#include "custom_browser/browser/nexus/tools/process_tool/nexus_shell_service_launcher.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"

namespace nexus {

namespace {

using base::Value;
using Dict = base::DictValue;

constexpr char kShellUnavailableCode[] = "SHELL_UNAVAILABLE";
constexpr char kShellUnavailableMessage[] =
    "shell APIs require an attached browser profile";

// The shells `shell.spawn` runs a command line with, by tool id, and the
// argv each takes — the same forms the Bash / PowerShell tools use
// (process_tool.cc BuildShellInvocation / BuildPowerShellInvocation), so
// the service resolves them against the same allowlist.
bool BuildSpawnInvocation(const std::string& shell,
                          const std::string& command_line,
                          std::string* command,
                          std::vector<std::string>* args) {
  if (shell == "bash" || shell == "sh") {
    *command = shell;
    *args = {"-c", command_line};
    return true;
  }
  if (shell == "pwsh" || shell == "powershell") {
    *command = shell;
    *args = {"-NoProfile", "-NonInteractive", "-Command", command_line};
    return true;
  }
  return false;
}

std::string EnsureUtf8(std::string text) {
  if (base::IsStringUTF8AllowingNoncharacters(text)) {
    return text;
  }
  return base::UTF16ToUTF8(base::UTF8ToUTF16(text));
}

}  // namespace

NexusShellBridgeAdapter::NexusShellBridgeAdapter(
    NexusBridgeResponseDelegate* delegate,
    Profile* profile)
    : ::nexus::generated::ShellApiBase(delegate), profile_(profile) {}

NexusShellBridgeAdapter::~NexusShellBridgeAdapter() = default;

void NexusShellBridgeAdapter::SetShellServiceForTesting(
    mojo::PendingRemote<mojom::NexusShellService> service) {
  service_for_testing_.reset();
  service_for_testing_.Bind(std::move(service));
}

mojom::NexusShellService* NexusShellBridgeAdapter::ShellService() {
  if (service_for_testing_.is_bound()) {
    return service_for_testing_.get();
  }
  if (!profile_) {
    return nullptr;
  }
  return GetSharedShellLauncher(profile_).GetService().get();
}

// One streaming child of `shell.spawn`: implements the service's client
// interface (output and exit pushes) and holds the controller remote. UI
// thread only, like the adapter.
class NexusShellBridgeAdapter::StreamingChild
    : public mojom::NexusStreamingProcessClient {
 public:
  StreamingChild(NexusShellBridgeAdapter* owner, std::string stream_id)
      : owner_(owner), stream_id_(std::move(stream_id)) {}
  ~StreamingChild() override = default;

  StreamingChild(const StreamingChild&) = delete;
  StreamingChild& operator=(const StreamingChild&) = delete;

  mojo::PendingReceiver<mojom::NexusStreamingProcess> BindController() {
    return controller_.BindNewPipeAndPassReceiver();
  }
  mojo::PendingRemote<mojom::NexusStreamingProcessClient> BindClient() {
    mojo::PendingRemote<mojom::NexusStreamingProcessClient> remote =
        client_receiver_.BindNewPipeAndPassRemote();
    // The service dropped its end (it died, or the child closed without an
    // exit push): the guest must still learn the stream is over.
    client_receiver_.set_disconnect_handler(base::BindOnce(
        &StreamingChild::OnDisconnected, base::Unretained(this)));
    return remote;
  }

  mojom::NexusStreamingProcess* controller() { return controller_.get(); }
  bool exited() const { return exited_; }

  // mojom::NexusStreamingProcessClient:
  void OnStdout(const std::vector<uint8_t>& data) override {
    owner_->PushStreamOutputEvent(stream_id_, "stdout", data);
  }
  void OnStderr(const std::vector<uint8_t>& data) override {
    owner_->PushStreamOutputEvent(stream_id_, "stderr", data);
  }
  void OnExit(mojom::ShellExitInfoPtr exit) override {
    if (exited_) return;
    exited_ = true;
    owner_->PushStreamExitEvent(stream_id_, *exit);
    owner_->OnStreamingChildGone(stream_id_);  // deletes `this`
  }

 private:
  void OnDisconnected() {
    if (exited_) return;
    exited_ = true;
    mojom::ShellExitInfo lost;
    lost.killed = true;
    owner_->PushStreamExitEvent(stream_id_, lost);
    owner_->OnStreamingChildGone(stream_id_);  // deletes `this`
  }

  const raw_ptr<NexusShellBridgeAdapter> owner_;
  const std::string stream_id_;
  bool exited_ = false;
  mojo::Remote<mojom::NexusStreamingProcess> controller_;
  mojo::Receiver<mojom::NexusStreamingProcessClient> client_receiver_{this};
};

void NexusShellBridgeAdapter::HandleSpawnOnUI(
    ::nexus::generated::SpawnInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  // The arguments are validated first, so a malformed call is INVALID_ARGS
  // whether or not a profile is attached. Exactly one form: the shell form {shell, command} (a command line the
  // shell parses) or the argv form {file, args} (run verbatim, no shell).
  const bool shell_form = input.shell.has_value() || input.command.has_value();
  const bool argv_form = input.file.has_value() || input.args.has_value();
  if (shell_form == argv_form) {
    std::move(done).InvalidArgs(
        "shell.spawn takes exactly one of {shell, command} or {file, args}");
    return;
  }
  auto options = mojom::ShellSpawnOptions::New();
  std::string display_command;
  if (shell_form) {
    const std::string shell = input.shell.value_or(std::string());
    const std::string command_line = input.command.value_or(std::string());
    if (!BuildSpawnInvocation(shell, command_line, &options->command,
                              &options->args)) {
      std::move(done).Error(
          "INVALID_SHELL",
          base::StringPrintf(
              "shell must be bash, sh, pwsh or powershell (got %s)",
              shell.c_str()));
      return;
    }
    if (command_line.empty()) {
      std::move(done).Error("INVALID_ARGS",
                            "command must be a non-empty string");
      return;
    }
    display_command = shell + " -c " + command_line;
  } else {
    if (!input.file.has_value() || input.file->empty()) {
      std::move(done).InvalidArgs("file must be a non-empty string");
      return;
    }
    // Resolved by the service against the same allowlist as every other
    // command it runs (a tool id, or an absolute path that canonicalises to
    // a resolved tool); `args` are passed verbatim.
    options->command = *input.file;
    options->args = input.args.value_or(std::vector<std::string>());
    std::vector<std::string> parts{options->command};
    parts.insert(parts.end(), options->args.begin(), options->args.end());
    display_command = base::JoinString(parts, " ");
  }
  // The same policy as the Bash tool's hook path: the shell is run
  // directly (use_shell=false), the service resolves the tool id, applies
  // its cwd policy and lays `env` over the host's.
  options->use_shell = false;
  if (input.cwd.has_value() && !input.cwd->empty()) {
    options->cwd = base::FilePath::FromUTF8Unsafe(*input.cwd);
  }
  if (input.env.has_value()) {
    // `{{secret:NAME}}` in a value is a secret stored with processEnv
    // (advertised as shell.spawn.secretEnv). Expanded here, into the options
    // the service gets; the plaintext never goes back to the guest.
    std::optional<NexusSecretStore> store;
    for (const auto entry : *input.env) {
      if (!entry.second.is_string() || entry.first.empty()) continue;
      auto env_entry = mojom::ShellEnvEntry::New();
      env_entry->name = entry.first;
      const std::string& value = entry.second.GetString();
      if (HasSecretPlaceholder(value)) {
        if (!store) {
          store.emplace(profile_ ? profile_->GetPrefs() : nullptr);
        }
        if (std::optional<SecretExpansionError> error =
                ExpandSecretPlaceholders(value, store->ProcessEnvLookup(),
                                         &env_entry->value)) {
          if (error->code == "INVALID_ARGS") {
            std::move(done).InvalidArgs(std::move(error->message));
          } else {
            std::move(done).Error(std::move(error->code),
                                  std::move(error->message));
          }
          return;
        }
      } else {
        env_entry->value = value;
      }
      options->env.push_back(std::move(env_entry));
    }
  }
  // `stdin_data` / `timeout_ms` / `max_output_bytes` are ignored by the
  // streaming path (RunStreamSpawn): the controller drives stdin and the
  // life of the child; the guest bounds what it keeps of the output.

  // `pty: {cols, rows}` runs the child under a pseudoconsole (advertised as
  // shell.spawn.pty). A console child is interactive by nature, so the
  // one-shot stdin fields and detaching make no sense with it.
  mojom::PtySizePtr pty_size;
  if (input.pty.has_value()) {
    if (input.persist.has_value() || input.stdinText.has_value() ||
        input.closeStdin.has_value()) {
      std::move(done).InvalidArgs(
          "pty cannot be combined with persist, stdinText or closeStdin");
      return;
    }
    const std::optional<int64_t> cols =
        bridge_support::FindIntegralValue(*input.pty, "cols");
    const std::optional<int64_t> rows =
        bridge_support::FindIntegralValue(*input.pty, "rows");
    if (!cols || !rows || *cols < 1 || *cols > kMaxPtyCells || *rows < 1 ||
        *rows > kMaxPtyCells) {
      std::move(done).InvalidArgs(
          "pty.cols and pty.rows must be integers within 1..1000");
      return;
    }
    pty_size = mojom::PtySize::New();
    pty_size->cols = static_cast<uint16_t>(*cols);
    pty_size->rows = static_cast<uint16_t>(*rows);
  }

  if (input.persist.has_value()) {
    // Detached from this connection: the profile-wide registry owns it and
    // its output goes to the log file (advertised as shell.spawn.persist).
    const std::string* log_path = input.persist->FindString("logPath");
    const base::FilePath log_file =
        log_path ? base::FilePath::FromUTF8Unsafe(*log_path) : base::FilePath();
    if (log_file.empty() || !log_file.IsAbsolute()) {
      std::move(done).InvalidArgs("persist.logPath must be an absolute path");
      return;
    }
    if (!profile_) {
      std::move(done).Error(kShellUnavailableCode, kShellUnavailableMessage);
      return;
    }
    GetPersistentShells(profile_).Start(
        std::move(options), log_file, std::move(display_command),
        input.stdinText, input.closeStdin.value_or(false),
        base::BindOnce(
            [](::nexus::BridgeResponseCallback done,
               NexusPersistentShells::StartResult result) {
              if (!result.ok) {
                std::move(done).Error(result.error_code, result.error_message);
                return;
              }
              Dict dict;
              dict.Set("persistentId", result.id);
              std::move(done).Success(std::move(dict));
            },
            std::move(done)));
    return;
  }

  mojom::NexusShellService* service = ShellService();
  if (!service) {
    std::move(done).Error(kShellUnavailableCode, kShellUnavailableMessage);
    return;
  }
  const std::string stream_id =
      base::StringPrintf("stream-%" PRIu64, ++next_stream_seq_);
  auto child = std::make_unique<StreamingChild>(this, stream_id);
  mojo::PendingReceiver<mojom::NexusStreamingProcess> controller =
      child->BindController();
  mojo::PendingRemote<mojom::NexusStreamingProcessClient> client =
      child->BindClient();
  // The initial stdin rides the controller: queued by the service until
  // the launch completes (StreamingChildProcess buffers pre-launch writes).
  if (input.stdinText.has_value() && !input.stdinText->empty()) {
    child->controller()->WriteStdin(
        std::vector<uint8_t>(input.stdinText->begin(), input.stdinText->end()));
  }
  if (input.closeStdin.value_or(false)) {
    child->controller()->CloseStdin();
  }
  streams_[stream_id] = std::move(child);

  auto on_done =
      base::BindOnce(&NexusShellBridgeAdapter::OnStreamSpawnDone,
                     weak_factory_.GetWeakPtr(), stream_id, std::move(done));
  if (pty_size) {
    service->StreamSpawnPty(std::move(options), std::move(pty_size),
                            std::move(controller), std::move(client),
                            std::move(on_done));
  } else {
    service->StreamSpawn(std::move(options), std::move(controller),
                         std::move(client), std::move(on_done));
  }
}

void NexusShellBridgeAdapter::OnStreamSpawnDone(
    std::string stream_id,
    ::nexus::BridgeResponseCallback done,
    bool ok,
    const std::optional<std::string>& error_code,
    const std::optional<std::string>& error_message,
    const std::optional<std::string>& install_hint) {
  if (!ok) {
    streams_.erase(stream_id);
    std::string message = error_message.value_or("the process could not start");
    if (install_hint.has_value() && !install_hint->empty()) {
      message += " (" + *install_hint + ")";
    }
    std::move(done).Error(error_code.value_or("SPAWN_FAILED"), message);
    return;
  }
  Dict dict;
  dict.Set("streamId", stream_id);
  std::move(done).Success(std::move(dict));
}

void NexusShellBridgeAdapter::PushStreamOutputEvent(
    const std::string& stream_id,
    const char* stream,
    const std::vector<uint8_t>& data) {
  if (data.empty()) return;
  Dict route;
  route.Set("namespace", "nexus");
  route.Set("capability", "shell");
  route.Set("action", "onStreamOutput");
  Dict payload;
  payload.Set("streamId", stream_id);
  payload.Set("stream", stream);
  // Raw bytes, base64: a chunk may end inside a multi-byte character, and
  // the guest's streaming TextDecoder joins it with the next one.
  payload.Set("dataBase64", base::Base64Encode(data));
  SendEvent(route, base::Value(std::move(payload)));
}

void NexusShellBridgeAdapter::PushStreamExitEvent(
    const std::string& stream_id,
    const mojom::ShellExitInfo& exit) {
  Dict route;
  route.Set("namespace", "nexus");
  route.Set("capability", "shell");
  route.Set("action", "onStreamExit");
  Dict payload;
  payload.Set("streamId", stream_id);
  payload.Set("exitCode", exit.code.value_or(-1));
  if (exit.signal.has_value()) {
    payload.Set("signal", *exit.signal);
  }
  payload.Set("timedOut", exit.timed_out);
  payload.Set("killed", exit.killed);
  payload.Set("durationMs", static_cast<double>(exit.duration_ms));
  SendEvent(route, base::Value(std::move(payload)));
}

void NexusShellBridgeAdapter::OnStreamingChildGone(const std::string& stream_id) {
  // Called from inside the child's own method: erase after this frame.
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) return;
  std::unique_ptr<StreamingChild> gone = std::move(it->second);
  streams_.erase(it);
  base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(FROM_HERE,
                                                              std::move(gone));
}

void NexusShellBridgeAdapter::HandleWriteStdinOnUI(
    ::nexus::generated::WriteStdinInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  auto it = streams_.find(input.streamId);
  Dict dict;
  if (it == streams_.end() || it->second->exited()) {
    dict.Set("found", false);
    std::move(done).Success(std::move(dict));
    return;
  }
  if (!input.data.empty()) {
    it->second->controller()->WriteStdin(input.data);
  }
  dict.Set("found", true);
  std::move(done).Success(std::move(dict));
}

void NexusShellBridgeAdapter::HandleCloseStdinOnUI(
    ::nexus::generated::CloseStdinInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  auto it = streams_.find(input.streamId);
  Dict dict;
  const bool found = it != streams_.end() && !it->second->exited();
  if (found) {
    it->second->controller()->CloseStdin();
  }
  dict.Set("found", found);
  std::move(done).Success(std::move(dict));
}

void NexusShellBridgeAdapter::HandleStreamKillOnUI(
    ::nexus::generated::StreamKillInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  auto it = streams_.find(input.streamId);
  Dict dict;
  const bool found = it != streams_.end() && !it->second->exited();
  if (found) {
    // The exit arrives through the client (killed=true) and erases the entry.
    it->second->controller()->Kill();
  }
  dict.Set("found", found);
  std::move(done).Success(std::move(dict));
}

void NexusShellBridgeAdapter::HandleResizeOnUI(
    ::nexus::generated::ShellResizeInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  if (input.cols < 1 || input.cols > kMaxPtyCells || input.rows < 1 ||
      input.rows > kMaxPtyCells) {
    std::move(done).InvalidArgs("cols and rows must be within 1..1000");
    return;
  }
  auto it = streams_.find(input.streamId);
  Dict dict;
  const bool found = it != streams_.end() && !it->second->exited();
  if (found) {
    // A pipe child ignores this in the service.
    auto size = mojom::PtySize::New();
    size->cols = static_cast<uint16_t>(input.cols);
    size->rows = static_cast<uint16_t>(input.rows);
    it->second->controller()->Resize(std::move(size));
  }
  dict.Set("found", found);
  std::move(done).Success(std::move(dict));
}

void NexusShellBridgeAdapter::HandlePersistentListOnUI(
    ::nexus::generated::ShellPersistentListInput /*input*/,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  if (!profile_) {
    std::move(done).Error(kShellUnavailableCode, kShellUnavailableMessage);
    return;
  }
  base::ListValue shells;
  for (const NexusPersistentShells::Record& record :
       GetPersistentShells(profile_).List()) {
    Dict row;
    row.Set("id", record.id);
    row.Set("command", EnsureUtf8(record.command));
    row.Set("logPath", record.log_path);
    row.Set("startedMs", record.started_ms);
    row.Set("running", record.running);
    if (record.exit_code.has_value()) {
      row.Set("exitCode", *record.exit_code);
    }
    if (record.signal.has_value()) {
      row.Set("signal", *record.signal);
    }
    if (!record.running) {
      row.Set("killed", record.killed);
      row.Set("timedOut", record.timed_out);
    }
    if (record.ended_ms.has_value()) {
      row.Set("endedMs", *record.ended_ms);
    }
    if (record.duration_ms.has_value()) {
      row.Set("durationMs", *record.duration_ms);
    }
    shells.Append(std::move(row));
  }
  Dict dict;
  dict.Set("shells", std::move(shells));
  std::move(done).Success(std::move(dict));
}

void NexusShellBridgeAdapter::HandlePersistentKillOnUI(
    ::nexus::generated::ShellPersistentKillInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  if (!profile_) {
    std::move(done).Error(kShellUnavailableCode, kShellUnavailableMessage);
    return;
  }
  const std::pair<bool, bool> killed =
      GetPersistentShells(profile_).Kill(input.id);
  Dict dict;
  dict.Set("found", killed.first);
  dict.Set("wasRunning", killed.second);
  std::move(done).Success(std::move(dict));
}

void NexusShellBridgeAdapter::HandleEnvOnUI(
    ::nexus::generated::ShellEnvInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  if (input.names.size() > kMaxEnvNames) {
    std::move(done).InvalidArgs(
        base::StringPrintf("shell.env: at most %zu names", kMaxEnvNames));
    return;
  }
  // Read-only and allowlist-free by design: the browser process's own
  // environment, which any local program of this user can read anyway.
  std::unique_ptr<base::Environment> env = base::Environment::Create();
  Dict values;
  for (const std::string& name : input.names) {
    if (name.empty()) {
      continue;
    }
    std::optional<std::string> value = env->GetVar(name);
    if (value.has_value()) {
      values.Set(name, EnsureUtf8(std::move(*value)));
    } else {
      values.Set(name, Value());
    }
  }
  Dict dict;
  dict.Set("values", std::move(values));
  std::move(done).Success(std::move(dict));
}

}  // namespace nexus
