// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/shell/nexus_shell_bridge_adapter.h"

#include <stdint.h>

#include <optional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "custom_browser/browser/nexus/mojom/nexus_shell.mojom.h"
#include "custom_browser/browser/nexus/tools/process_tool/nexus_shell_service_launcher.h"
#include "mojo/public/cpp/base/big_buffer.h"

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
    : ::nexus::generated::ShellApiBase(delegate), profile_(profile) {}

NexusShellBridgeAdapter::~NexusShellBridgeAdapter() = default;

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

}  // namespace nexus
