// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/tools/nexus_ripgrep_runner.h"

#include <algorithm>
#include <utility>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/scoped_refptr.h"
#include "base/strings/strcat.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/storage_partition.h"
#include "custom_browser/browser/nexus/binaries/nexus_binary_installer.h"
#include "custom_browser/browser/nexus/tools/process_tool/nexus_shell_service_launcher.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"

namespace nexus {

namespace {

// Ensures `text` is valid UTF-8, round-tripping through UTF-16 to substitute
// invalid sequences with U+FFFD on the slow path. rg is UTF-8-aware on
// matched content, but file paths / binary matches can still carry non-UTF-8
// bytes, and base::Value DCHECKs UTF-8 on the way back out. Mirrors the
// identical helper in process_tool.cc (kept file-local on both sides to avoid
// a cross-cutting "string utils" dependency).
std::string EnsureUtf8(std::string text) {
  if (base::IsStringUTF8AllowingNoncharacters(text)) {
    return text;
  }
  return base::UTF16ToUTF8(base::UTF8ToUTF16(text));
}

// Parse rg's stdout into a vector of trimmed, CRLF-stripped, non-empty lines.
// rg exits 1 when there are no matches — success from the caller's POV (empty
// vector). Any other non-zero exit is a real error; we still return whatever
// rg printed (best-effort). Mirrors the result-shaping that used to live in
// process_tool.cc's `ParseRipgrepLines`.
std::vector<std::string> ParseRipgrepLines(
    const mojo_base::BigBuffer& stdout_buffer,
    int exit_code,
    size_t cap) {
  if (exit_code == 1) {
    return {};
  }
  base::span<const uint8_t> bytes(stdout_buffer);
  size_t n = std::min(bytes.size(), cap);
  if (n == 0) {
    return {};
  }
  std::string text =
      EnsureUtf8(std::string(reinterpret_cast<const char*>(bytes.data()), n));
  std::string trimmed;
  base::TrimString(text, "\r\n \t", &trimmed);
  if (trimmed.empty()) {
    return {};
  }
  std::vector<std::string> lines;
  for (const std::string& line : base::SplitString(
           trimmed, "\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    if (!line.empty() && line.back() == '\r') {
      lines.push_back(line.substr(0, line.size() - 1));
    } else {
      lines.push_back(line);
    }
  }
  return lines;
}

// Worker-thread parse of a launched rg result into a RipgrepRunResult. Runs on
// a base::ThreadPool worker (no `this`) so the SplitString + per-line
// allocation over rg's stdout (capped, but up to several MiB for a broad glob)
// never burns the UI thread. Mirrors FileTool's static `<Name>Blocking` helper
// pattern.
RipgrepRunResult ParseExecResultBlocking(mojom::ShellExecResultPtr result,
                                         size_t cap) {
  RipgrepRunResult run_result;
  run_result.launched = true;
  int exit_code =
      result->exit && result->exit->code.has_value() ? *result->exit->code : 0;
  run_result.exit_code = exit_code;
  run_result.lines = ParseRipgrepLines(result->stdout_buffer, exit_code, cap);
  return run_result;
}

// Canonical "rg is unavailable and we could not provision it" message,
// shared by the pre-launch-failure and failed-auto-download paths so both
// Grep and Glob surface the same manual-install recovery hint.
std::string BuildInstallHintMessage(const std::string& detail) {
  std::string msg =
      "ripgrep (rg) is required and the automatic download did not produce a "
      "working binary";
  if (!detail.empty()) {
    msg = base::StrCat({msg, " — ", detail});
  }
  return base::StrCat({msg,
                       ".\n\nInstall it manually and retry:\n"
                       "  winget install --id BurntSushi.ripgrep.MSVC\n"
                       "  choco install ripgrep -y\n"
                       "  scoop install ripgrep"});
}

}  // namespace

// =============================================================================
// RipgrepRunResult out-of-line special members (chromium-style).
// =============================================================================

RipgrepRunResult::RipgrepRunResult() = default;
RipgrepRunResult::RipgrepRunResult(RipgrepRunResult&&) noexcept = default;
RipgrepRunResult& RipgrepRunResult::operator=(RipgrepRunResult&&) noexcept =
    default;
RipgrepRunResult::~RipgrepRunResult() = default;

// =============================================================================
// NexusRipgrepRunner
// =============================================================================

NexusRipgrepRunner::NexusRipgrepRunner() = default;
NexusRipgrepRunner::~NexusRipgrepRunner() = default;

// static
RipgrepRunResult NexusRipgrepRunner::ParseExecResultForTesting(
    mojom::ShellExecResultPtr result,
    size_t cap) {
  return ParseExecResultBlocking(std::move(result), cap);
}

void NexusRipgrepRunner::Reset() {
  weak_ptr_factory_.InvalidateWeakPtrs();
}

void NexusRipgrepRunner::Run(NexusShellServiceLauncher* launcher,
                             Profile* profile,
                             std::vector<std::string> rg_args,
                             RipgrepRunOptions options,
                             ResultCallback callback) {
  // First attempt resolves `rg` from PATH (a system install). If that comes
  // back BINARY_NOT_FOUND, OnExecDone auto-installs the pinned ripgrep into
  // ~/.velloc/bin and retries once against its absolute path — so a download
  // only happens when rg isn't already on the host.
  Launch(launcher, profile, /*command=*/"rg", std::move(rg_args),
         std::move(options), /*allow_install_retry=*/true, std::move(callback));
}

void NexusRipgrepRunner::Launch(NexusShellServiceLauncher* launcher,
                                Profile* profile,
                                std::string command,
                                std::vector<std::string> rg_args,
                                RipgrepRunOptions options,
                                bool allow_install_retry,
                                ResultCallback callback) {
  auto spawn_options = mojom::ShellSpawnOptions::New();
  spawn_options->command = std::move(command);
  // Copy the argv so the original survives for a possible post-install retry.
  spawn_options->args = rg_args;
  spawn_options->cwd = options.cwd;
  spawn_options->use_shell = false;
  spawn_options->timeout_ms = options.timeout_ms;
  spawn_options->max_output_bytes = options.max_output_bytes;

  mojo::Remote<mojom::NexusShellService>& remote = launcher->GetService();
  remote->Exec(std::move(spawn_options),
               base::BindOnce(&NexusRipgrepRunner::OnExecDone,
                              weak_ptr_factory_.GetWeakPtr(), launcher, profile,
                              std::move(rg_args), std::move(options),
                              allow_install_retry, std::move(callback)));
}

void NexusRipgrepRunner::OnExecDone(
    NexusShellServiceLauncher* launcher,
    Profile* profile,
    std::vector<std::string> rg_args,
    RipgrepRunOptions options,
    bool allow_install_retry,
    ResultCallback callback,
    bool ok,
    mojom::ShellExecResultPtr result,
    const std::optional<std::string>& error_code,
    const std::optional<std::string>& error_message,
    const std::optional<std::string>& install_hint) {
  // Pre-launch failure path. ripgrep missing from the host shows up here as
  // BINARY_NOT_FOUND.
  if (!ok || !result) {
    // On the FIRST attempt, auto-install the pinned ripgrep into
    // ~/.velloc/bin and retry once against its absolute path instead of
    // surfacing the error. A download only happens here — i.e. only when rg
    // wasn't already resolvable on PATH.
    if (allow_install_retry && error_code.has_value() &&
        *error_code == "BINARY_NOT_FOUND" && profile) {
      LOG(WARNING) << "Ripgrep not on PATH; attempting managed auto-download "
                      "into ~/.velloc/bin";
      scoped_refptr<network::SharedURLLoaderFactory> factory =
          profile->GetDefaultStoragePartition()
              ->GetURLLoaderFactoryForBrowserProcess();
      NexusBinaryInstaller::GetInstance()->EnsureBinary(
          std::move(factory), "rg",
          base::BindOnce(&NexusRipgrepRunner::OnRgEnsured,
                         weak_ptr_factory_.GetWeakPtr(), launcher,
                         std::move(rg_args), std::move(options),
                         std::move(callback)));
      return;
    }
    RipgrepRunResult run_result;
    run_result.launched = false;
    run_result.error_code = error_code.value_or("ripgrep_unavailable");
    std::string msg =
        error_message.has_value()
            ? base::StrCat({"ripgrep unavailable: ", *error_message})
            : std::string("ripgrep unavailable");
    if (install_hint.has_value() && !install_hint->empty()) {
      msg = base::StrCat({msg, "\n\n", *install_hint});
    }
    run_result.error_message = std::move(msg);
    std::move(callback).Run(std::move(run_result));
    return;
  }

  // Launched — `result` carries rg's captured stdout + exit code. Parsing the
  // (capped, but up-to-MiBs) buffer into lines is real CPU, so hop it to a
  // base::ThreadPool worker (static helper, no `this`) and resolve from a
  // weak-gated UI reply — keeping the UI thread free of the work, the same
  // pattern FileTool's Read/Write/Edit use for their blocking I/O.
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&ParseExecResultBlocking, std::move(result),
                     options.max_output_bytes),
      base::BindOnce(&NexusRipgrepRunner::OnParsed,
                     weak_ptr_factory_.GetWeakPtr(), std::move(callback)));
}

void NexusRipgrepRunner::OnParsed(ResultCallback callback,
                                  RipgrepRunResult result) {
  std::move(callback).Run(std::move(result));
}

void NexusRipgrepRunner::OnRgEnsured(NexusShellServiceLauncher* launcher,
                                     std::vector<std::string> rg_args,
                                     RipgrepRunOptions options,
                                     ResultCallback callback,
                                     const EnsureBinaryResult& ensured) {
  if (ensured.ok()) {
    // Exec the absolute managed rg (SafeCommandPolicy allows it because it
    // lives under ~/.velloc/bin). No further install retry, and no profile
    // needed (a second BINARY_NOT_FOUND can't be recovered).
    Launch(launcher, /*profile=*/nullptr, ensured.path.AsUTF8Unsafe(),
           std::move(rg_args), std::move(options),
           /*allow_install_retry=*/false, std::move(callback));
    return;
  }
  // The auto-download was attempted but did not produce a working rg. Surface
  // WHY (the URL it tried, the failing step) so a failed auto-download is
  // diagnosable rather than looking identical to "rg never installed".
  LOG(WARNING) << "Ripgrep auto-download did not yield a binary: "
               << ensured.detail;
  RipgrepRunResult run_result;
  run_result.launched = false;
  run_result.error_code = "ripgrep_unavailable";
  run_result.error_message = BuildInstallHintMessage(ensured.detail);
  std::move(callback).Run(std::move(run_result));
}

}  // namespace nexus
