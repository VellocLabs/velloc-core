// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

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
#include "custom_browser/browser/nexus/api/shell/nexus_persistent_shells.h"
#include "custom_browser/browser/nexus/mojom/nexus_shell.mojom.h"
#include "custom_browser/browser/nexus/tools/process_tool/nexus_shell_service_launcher.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"

namespace nexus {

namespace {

using base::Value;
using Dict = base::DictValue;

// UI-THREAD SAFETY — defense in depth.
//
// shell.status's reply is marshaled on the BROWSER UI thread: this string copy
// runs on the UI-bound mojo reply, and the bridge then JSON-serializes the dict
// (incl. this output string) on UI in
// NexusBridgePortService::SendSuccessResponse (an unavoidable framework
// invariant for every JSON bridge response). That is fine ONLY because the
// output is bounded: ProcessTool hard-caps every background spawn's output at
// 30 KB/stream (process_tool.cc `kMaxOutputBytes`), far below a frame budget.
//
// But that safety currently depends on a DISTANT caller's constant. Clamp here
// too — independent of ProcessTool — so a future cap increase
// (SafeCommandPolicy's ceiling is 16 MiB) can NEVER make this reply
// walk/serialize megabytes on the UI thread. The drawer detail only needs a
// preview; `*clamped` is OR'd into the stream's truncated flag so the guest
// knows the tail was dropped. With today's 30 KB producer cap this clamp never
// triggers (no behavior change).
constexpr size_t kMaxStatusStreamBytes = 256u * 1024u;
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

// Decode up to kMaxStatusStreamBytes of a captured-output BigBuffer as a UTF-8
// string for the JSON reply; sets *clamped if the buffer was longer. Shell
// output is text; a non-UTF-8 byte sequence degrades lossily, which is fine for
// the drawer's display. `base::Value` requires valid UTF-8, so normalizing here
// keeps background status replies from failing on legacy console encodings.
// (Contrast fs.readFileBytes, which returns raw bytes over a binary port because
// callers may read arbitrary binary files.)
std::string ClampedBigBufferToString(const mojo_base::BigBuffer& buffer,
                                     bool* clamped) {
  const size_t n = buffer.size() < kMaxStatusStreamBytes
                       ? buffer.size()
                       : kMaxStatusStreamBytes;
  if (clamped) {
    *clamped = buffer.size() > kMaxStatusStreamBytes;
  }
  if (n == 0) {
    return std::string();
  }
  return EnsureUtf8(std::string(reinterpret_cast<const char*>(buffer.data()), n));
}

NexusShellServiceLauncher::CachedBackgroundOutput BuildCachedOutput(
    const mojom::ShellBackgroundOutput& output,
    const std::string& stdout_text,
    const std::string& stderr_text,
    bool stdout_truncated,
    bool stderr_truncated) {
  NexusShellServiceLauncher::CachedBackgroundOutput cached;
  if (output.exit) {
    cached.exit_code = output.exit->code.value_or(-1);
    cached.signal = output.exit->signal;
    cached.timed_out = output.exit->timed_out;
    cached.killed = output.exit->killed;
    cached.duration_ms = output.exit->duration_ms;
  }
  cached.stdout_text = stdout_text;
  cached.stderr_text = stderr_text;
  cached.stdout_truncated = stdout_truncated;
  cached.stderr_truncated = stderr_truncated;
  return cached;
}

// Reply shaper for shell.status. Free function (touches only `done` + the
// reply) so it's safe even if the adapter was destroyed mid-call.
void RespondStatus(base::WeakPtr<NexusShellServiceLauncher> launcher,
                   std::string shell_id,
                   BridgeResponseCallback done,
                   mojom::ShellBackgroundOutputPtr output) {
  Dict dict;
  if (!output || !output->found) {
    if (launcher) {
      std::optional<NexusShellServiceLauncher::CachedBackgroundOutput> cached =
          launcher->GetCachedBackgroundOutput(shell_id);
      launcher->ReleaseBackgroundShell(shell_id);
      if (cached.has_value()) {
        dict.Set("found", true);
        dict.Set("running", false);
        dict.Set("stdout", cached->stdout_text);
        dict.Set("stderr", cached->stderr_text);
        dict.Set("stdoutTruncated", cached->stdout_truncated);
        dict.Set("stderrTruncated", cached->stderr_truncated);
        dict.Set("exitCode", cached->exit_code);
        if (cached->signal.has_value()) {
          dict.Set("signal", *cached->signal);
        }
        dict.Set("timedOut", cached->timed_out);
        dict.Set("killed", cached->killed);
        dict.Set("durationMs", static_cast<double>(cached->duration_ms));
        std::move(done).Success(std::move(dict));
        return;
      }
    }
    dict.Set("found", false);
    dict.Set("running", false);
    dict.Set("stdout", "");
    dict.Set("stderr", "");
    dict.Set("stdoutTruncated", false);
    dict.Set("stderrTruncated", false);
    std::move(done).Success(std::move(dict));
    return;
  }

  dict.Set("found", output->found);
  dict.Set("running", output->running);
  bool stdout_clamped = false;
  bool stderr_clamped = false;
  const std::string stdout_text =
      ClampedBigBufferToString(output->stdout_buffer, &stdout_clamped);
  const std::string stderr_text =
      ClampedBigBufferToString(output->stderr_buffer, &stderr_clamped);
  const bool stdout_truncated = output->stdout_truncated || stdout_clamped;
  const bool stderr_truncated = output->stderr_truncated || stderr_clamped;
  dict.Set("stdout", stdout_text);
  dict.Set("stderr", stderr_text);
  dict.Set("stdoutTruncated", stdout_truncated);
  dict.Set("stderrTruncated", stderr_truncated);
  // `exit` is null while running; populated once the process has exited/killed.
  if (output->exit) {
    if (output->exit->code.has_value()) {
      dict.Set("exitCode", *output->exit->code);
    }
    if (output->exit->signal.has_value()) {
      dict.Set("signal", *output->exit->signal);
    }
    dict.Set("timedOut", output->exit->timed_out);
    dict.Set("killed", output->exit->killed);
    // duration_ms is uint64; base::Value has no 64-bit int — use double (a
    // millisecond duration is well within double's exact-integer range).
    dict.Set("durationMs", static_cast<double>(output->exit->duration_ms));
  }
  if (!output->running && launcher) {
    launcher->CacheBackgroundOutput(
        shell_id, BuildCachedOutput(*output, stdout_text, stderr_text,
                                    stdout_truncated, stderr_truncated));
    launcher->ReleaseBackgroundShell(shell_id);
  }
  std::move(done).Success(std::move(dict));
}

// Reply shaper for shell.kill.
void RespondKill(BridgeResponseCallback done, bool found, bool was_running) {
  Dict dict;
  dict.Set("found", found);
  dict.Set("wasRunning", was_running);
  std::move(done).Success(std::move(dict));
}

}  // namespace

NexusShellBridgeAdapter::NexusShellBridgeAdapter(
    NexusBridgeResponseDelegate* delegate,
    Profile* profile)
    : ::nexus::generated::ShellApiBase(delegate), profile_(profile) {
  // Register for background-shell terminal exits for the lifetime of this
  // adapter. The launcher broadcasts every shell's exit here; we push a
  // `shell.onExit` bridge event so nexus_web finalizes the `local_bash` task
  // event-driven instead of polling shell.status. Weak-guarded so a teardown
  // racing an in-flight exit drops cleanly.
  if (profile_) {
    shell_event_sink_id_ = GetSharedShellLauncher(profile_).AddShellEventSink(
        base::BindRepeating(&NexusShellBridgeAdapter::PushShellExitEvent,
                            weak_factory_.GetWeakPtr()));
  }
}

NexusShellBridgeAdapter::~NexusShellBridgeAdapter() {
  if (profile_ && shell_event_sink_id_ != 0) {
    GetSharedShellLauncher(profile_).RemoveShellEventSink(shell_event_sink_id_);
  }
}

void NexusShellBridgeAdapter::PushShellExitEvent(
    const std::string& shell_id,
    const mojom::ShellExitInfo& exit) {
  // Mirror the TS-side `shell.onExit` event envelope; the transport wraps this
  // as {claudeBridge:true, direction:"event", ...}. Payload mirrors the
  // shell.status terminal fields so the guest reuses its existing exit
  // classification (classifyShellExit) verbatim.
  Dict route;
  route.Set("namespace", "nexus");
  route.Set("capability", "shell");
  route.Set("action", "onExit");

  Dict payload;
  payload.Set("shellId", shell_id);
  payload.Set("exitCode", exit.code.value_or(-1));
  if (exit.signal.has_value()) {
    payload.Set("signal", *exit.signal);
  }
  payload.Set("timedOut", exit.timed_out);
  payload.Set("killed", exit.killed);
  // duration_ms is uint64; base::Value has no 64-bit int — use double (a
  // millisecond duration is well within double's exact-integer range), matching
  // the shell.status reply shaper.
  payload.Set("durationMs", static_cast<double>(exit.duration_ms));
  SendEvent(route, base::Value(std::move(payload)));
}

void NexusShellBridgeAdapter::HandleStatusOnUI(
    ::nexus::generated::StatusInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  if (!profile_) {
    std::move(done).Error(kShellUnavailableCode, kShellUnavailableMessage);
    return;
  }
  // Fire an async poll on the SHARED launcher's remote (same utility process +
  // registry ProcessTool spawned into). The reply lands on UI.
  NexusShellServiceLauncher& launcher = GetSharedShellLauncher(profile_);
  std::string shell_id = std::move(input.shellId);
  launcher.GetService()->ReadBackgroundOutput(
      shell_id, base::BindOnce(&RespondStatus, launcher.GetWeakPtr(), shell_id,
                               std::move(done)));
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
    for (const auto entry : *input.env) {
      if (!entry.second.is_string() || entry.first.empty()) continue;
      auto env_entry = mojom::ShellEnvEntry::New();
      env_entry->name = entry.first;
      env_entry->value = entry.second.GetString();
      options->env.push_back(std::move(env_entry));
    }
  }
  // `stdin_data` / `timeout_ms` / `max_output_bytes` are ignored by the
  // streaming path (RunStreamSpawn): the controller drives stdin and the
  // life of the child; the guest bounds what it keeps of the output.
  options->background_on_timeout = false;

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

  if (!profile_) {
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

  GetSharedShellLauncher(profile_).GetService()->StreamSpawn(
      std::move(options), std::move(controller), std::move(client),
      base::BindOnce(&NexusShellBridgeAdapter::OnStreamSpawnDone,
                     weak_factory_.GetWeakPtr(), stream_id, std::move(done)));
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

void NexusShellBridgeAdapter::HandleKillOnUI(
    ::nexus::generated::KillInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  if (!profile_) {
    std::move(done).Error(kShellUnavailableCode, kShellUnavailableMessage);
    return;
  }
  GetSharedShellLauncher(profile_).GetService()->Kill(
      std::move(input.shellId), base::BindOnce(&RespondKill, std::move(done)));
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
