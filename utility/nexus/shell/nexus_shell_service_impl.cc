// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/utility/nexus/shell/nexus_shell_service_impl.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>
#include <vector>

#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/strcat.h"
#include "base/task/bind_post_task.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "base/uuid.h"
#include "build/build_config.h"
#include "custom_browser/browser/nexus/mojom/nexus_shell.mojom.h"
#include "custom_browser/utility/nexus/shell/child_process_runner.h"
#include "custom_browser/utility/nexus/shell/safe_command_policy.h"
#include "mojo/public/cpp/base/big_buffer.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>
#elif BUILDFLAG(IS_POSIX)
#include <signal.h>
#endif

namespace nexus {

namespace {

mojom::ShellResolvedToolPtr ToMojom(const ResolvedTool& tool) {
  auto out = mojom::ShellResolvedTool::New();
  out->id = tool.id;
  out->path = tool.path;
  out->version = tool.version;
  out->available = tool.available;
  return out;
}

mojo_base::BigBuffer BytesToBigBuffer(const std::vector<uint8_t>& bytes) {
  return mojo_base::BigBuffer(base::span(bytes));
}

mojom::ShellExecResultPtr ToMojomExecResult(const ChildProcessOutcome& out,
                                            base::TimeDelta total_duration) {
  auto result = mojom::ShellExecResult::New();
  auto exit = mojom::ShellExitInfo::New();
  exit->code = out.code;
  exit->signal = out.signal;
  exit->timed_out = out.timed_out;
  exit->killed = out.killed;
  exit->duration_ms = static_cast<uint64_t>(total_duration.InMilliseconds());
  result->exit = std::move(exit);
  result->stdout_buffer = BytesToBigBuffer(out.stdout_bytes);
  result->stderr_buffer = BytesToBigBuffer(out.stderr_bytes);
  result->stdout_truncated = out.stdout_truncated;
  result->stderr_truncated = out.stderr_truncated;
  return result;
}

void RunErrorReply(NexusShellServiceImpl::ExecCallback callback,
                   const std::string& code,
                   const std::string& message,
                   const std::string& install_hint = std::string()) {
  std::optional<std::string> hint;
  if (!install_hint.empty()) {
    hint = install_hint;
  }
  std::move(callback).Run(/*ok=*/false, /*result=*/nullptr, code, message,
                          hint);
}

// Cap on tracked background processes. Oldest COMPLETED entry is evicted
// when a new Spawn would exceed this. Running entries are never evicted
// (we'd lose their pending outcome), so the map can transiently exceed
// the cap if every slot is an in-flight process — bounded in practice by
// how many background commands a session realistically launches at once.
constexpr size_t kMaxBackgroundEntries = 32;

bool TerminateBackgroundTree(ChildProcessKillHandle& kill_handle) {
#if BUILDFLAG(IS_WIN)
  if (kill_handle.job.is_valid()) {
    const bool ok = ::TerminateJobObject(kill_handle.job.get(),
                                         static_cast<UINT>(-1)) != FALSE;
    if (!ok) {
      PLOG(WARNING) << "[NexusShellService] TerminateJobObject failed";
    }
    return ok;
  }
  if (!kill_handle.process.IsValid()) {
    return false;
  }
  return kill_handle.process.Terminate(/*exit_code=*/-1, /*wait=*/false);
#else
  if (kill_handle.process_group_id == base::kNullProcessId) {
    return false;
  }
  const bool ok = ::killpg(kill_handle.process_group_id, SIGKILL) == 0;
  if (!ok) {
    PLOG(WARNING) << "[NexusShellService] killpg(" << kill_handle.process_group_id
                  << ", SIGKILL) failed";
  }
  return ok;
#endif
}

// Outcome of validating + resolving a ShellSpawnOptions into a runnable
// ChildProcessRequest. Shared by the foreground `Exec` and background
// `Spawn` paths so they apply identical cwd / allowlist / shell-rewrite /
// env / timeout policy. On `ok == false`, `error_code` / `error_message`
// (+ `install_hint` for BINARY_NOT_FOUND) describe the pre-launch
// rejection and `request` is unset.
struct PreparedExec {
  bool ok = false;
  std::string error_code;
  std::string error_message;
  std::string install_hint;
  ChildProcessRequest request;
};

#if BUILDFLAG(IS_WIN)
// Locates cmd.exe so batch wrappers (`npx.cmd`, `npm.cmd`, …) can be launched
// through it. Prefers the resolver's `cmd` entry, then %ComSpec%, then the
// canonical System32 path. Returns an empty path if none resolve.
base::FilePath FindCmdExe(const ResolvedCapabilities& caps) {
  for (const ResolvedTool& tool : caps.tools) {
    if (tool.id == "cmd" && tool.available && !tool.path.empty()) {
      return tool.path;
    }
  }
  std::unique_ptr<base::Environment> env = base::Environment::Create();
  if (std::optional<std::string> comspec = env->GetVar("ComSpec");
      comspec.has_value() && !comspec->empty()) {
    return base::FilePath::FromUTF8Unsafe(*comspec);
  }
  if (std::optional<std::string> sysroot = env->GetVar("SystemRoot");
      sysroot.has_value() && !sysroot->empty()) {
    return base::FilePath::FromUTF8Unsafe(*sysroot)
        .AppendASCII("System32")
        .AppendASCII("cmd.exe");
  }
  return base::FilePath();
}
#endif  // BUILDFLAG(IS_WIN)

PreparedExec PrepareExec(const mojom::ShellSpawnOptions& options,
                         const ResolvedCapabilities& caps) {
  PreparedExec prep;

  // Step 1: validate cwd (most-specific error first if cwd + command
  // are both wrong).
  std::string cwd_reason;
  if (!SafeCommandPolicy::ValidateCwd(options.cwd, &cwd_reason)) {
    prep.error_code = "INVALID_CWD";
    prep.error_message = cwd_reason;
    return prep;
  }

  // Step 2: resolve command (allowlist + default-shell rewrite).
  std::string final_command = options.command;
  std::vector<std::string> final_args = options.args;
  if (options.use_shell) {
    if (!caps.default_shell.has_value()) {
      prep.error_code = "NO_DEFAULT_SHELL";
      prep.error_message = "Host has no resolvable default shell on PATH";
      return prep;
    }
    const ResolvedTool& shell = *caps.default_shell;
    std::vector<std::string> shell_args;
    if (shell.id == "cmd") {
      shell_args.push_back("/C");
      shell_args.push_back(options.command);
    } else {
      shell_args.push_back("-c");
      shell_args.push_back(options.command);
      shell_args.push_back(shell.id);
      for (const std::string& arg : options.args) {
        shell_args.push_back(arg);
      }
    }
    final_command = shell.id;
    final_args = std::move(shell_args);
  }

  PolicyDecision decision =
      SafeCommandPolicy::ResolveCommand(final_command, caps);
  if (!decision.ok()) {
    prep.error_code = decision.error_code;
    prep.error_message = decision.error_message;
    prep.install_hint = decision.install_hint;
    return prep;
  }

  // Step 3: build the request.
  ChildProcessRequest request;
  request.exe = decision.resolved_path;
  request.args = std::move(final_args);

#if BUILDFLAG(IS_WIN)
  // CreateProcess cannot execute batch wrappers (`npx.cmd`, `npm.cmd`,
  // `pnpm.cmd`, …) — they are scripts, not PE images — so launching one
  // directly fails with LAUNCH_FAILED. Re-route through `cmd.exe /C` so
  // stdio MCP servers distributed as npm/uv wrappers start. The command +
  // args are configured by the trusted user in settings (not the LLM), so
  // we accept cmd.exe's own argument-parsing semantics for the forwarded
  // args. The allowlist decision above has already passed against the
  // wrapper's own id; this only changes HOW it's launched, not WHETHER.
  //
  // Note: relies on cmd.exe's /C "preserve quotes when the tail has exactly
  // two quotes around a valid executable" rule, which holds for the common
  // case (a possibly-spaced install path + space-free args). Args that
  // themselves contain spaces or cmd metacharacters are not robustly
  // quoted; that is a follow-up if it surfaces.
  if (request.exe.MatchesFinalExtension(FILE_PATH_LITERAL(".cmd")) ||
      request.exe.MatchesFinalExtension(FILE_PATH_LITERAL(".bat"))) {
    base::FilePath cmd_exe = FindCmdExe(caps);
    if (cmd_exe.empty()) {
      prep.error_code = "NO_COMMAND_INTERPRETER";
      prep.error_message =
          "cmd.exe is required to launch '" + request.exe.AsUTF8Unsafe() +
          "' but could not be located on this host";
      return prep;
    }
    std::vector<std::string> wrapped;
    wrapped.reserve(request.args.size() + 2);
    wrapped.push_back("/C");
    wrapped.push_back(request.exe.AsUTF8Unsafe());
    for (std::string& arg : request.args) {
      wrapped.push_back(std::move(arg));
    }
    request.exe = std::move(cmd_exe);
    request.args = std::move(wrapped);
  }
#endif  // BUILDFLAG(IS_WIN)

  request.cwd = options.cwd;
  request.stdin_data = options.stdin_data;

  std::vector<EnvOverride> env_overrides;
  env_overrides.reserve(options.env.size());
  for (const auto& entry : options.env) {
    env_overrides.push_back({entry->name, entry->value});
  }
  request.env = SafeCommandPolicy::BuildSafeEnvironment(env_overrides);
  SafeCommandPolicy::AugmentEnvironmentForResolvedCommand(
      final_command, request.exe, &request.env);
  request.timeout = base::Milliseconds(
      SafeCommandPolicy::ClampTimeoutMs(options.timeout_ms));
  request.max_output_bytes =
      SafeCommandPolicy::ClampMaxOutputBytes(options.max_output_bytes);

  prep.ok = true;
  prep.request = std::move(request);
  return prep;
}

}  // namespace

NexusShellServiceImpl::PendingExec::PendingExec() = default;

NexusShellServiceImpl::PendingExec::~PendingExec() = default;

NexusShellServiceImpl::PendingExec::PendingExec(PendingExec&&) = default;

NexusShellServiceImpl::PendingExec&
NexusShellServiceImpl::PendingExec::operator=(PendingExec&&) = default;

NexusShellServiceImpl::PendingSpawn::PendingSpawn() = default;

NexusShellServiceImpl::PendingSpawn::~PendingSpawn() = default;

NexusShellServiceImpl::PendingSpawn::PendingSpawn(PendingSpawn&&) = default;

NexusShellServiceImpl::PendingSpawn&
NexusShellServiceImpl::PendingSpawn::operator=(PendingSpawn&&) = default;

NexusShellServiceImpl::PendingStreamSpawn::PendingStreamSpawn() = default;

NexusShellServiceImpl::PendingStreamSpawn::~PendingStreamSpawn() = default;

NexusShellServiceImpl::PendingStreamSpawn::PendingStreamSpawn(
    PendingStreamSpawn&&) = default;

NexusShellServiceImpl::PendingStreamSpawn&
NexusShellServiceImpl::PendingStreamSpawn::operator=(PendingStreamSpawn&&) =
    default;

NexusShellServiceImpl::BackgroundEntry::BackgroundEntry() = default;

NexusShellServiceImpl::BackgroundEntry::~BackgroundEntry() = default;

NexusShellServiceImpl::BackgroundEntry::BackgroundEntry(BackgroundEntry&&) =
    default;

NexusShellServiceImpl::BackgroundEntry&
NexusShellServiceImpl::BackgroundEntry::operator=(BackgroundEntry&&) = default;

NexusShellServiceImpl::ExecBackgroundState::ExecBackgroundState() = default;

NexusShellServiceImpl::ExecBackgroundState::~ExecBackgroundState() = default;

NexusShellServiceImpl::NexusShellServiceImpl(
    mojo::PendingReceiver<mojom::NexusShellService> receiver) {
  receiver_.Bind(std::move(receiver));
}

NexusShellServiceImpl::~NexusShellServiceImpl() = default;

void NexusShellServiceImpl::GetCapabilities(
    GetCapabilitiesCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (cached_capabilities_.has_value()) {
    RespondFromCache(std::move(callback));
    return;
  }

  pending_callbacks_.push_back(std::move(callback));
  EnsureResolveStarted();
}

void NexusShellServiceImpl::Exec(mojom::ShellSpawnOptionsPtr options,
                                 ExecCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (!options) {
    RunErrorReply(std::move(callback), "INVALID_ARGS",
                  "Exec requires a non-null ShellSpawnOptions",
                  /*install_hint=*/std::string());
    return;
  }

  if (!cached_capabilities_.has_value()) {
    PendingExec pending;
    pending.options = std::move(options);
    pending.callback = std::move(callback);
    pending_execs_.push_back(std::move(pending));
    EnsureResolveStarted();
    return;
  }

  RunExec(std::move(options), std::move(callback));
}

void NexusShellServiceImpl::EnsureResolveStarted() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (resolve_in_flight_) {
    return;
  }
  resolve_in_flight_ = true;

  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      // WithBaseSyncPrimitives() is required because ShellResolver::Resolve
      // now runs the `--version` probe through RunChildProcess, which calls
      // base::Process::WaitForExitWithTimeout — Chromium classifies the
      // child-process wait as a "base sync primitive" and DCHECKs if the task
      // wasn't declared to allow it (same reason as the Exec path below).
      {base::MayBlock(), base::WithBaseSyncPrimitives()},
      base::BindOnce(&ShellResolver::Resolve),
      base::BindOnce(&NexusShellServiceImpl::OnResolved,
                     weak_factory_.GetWeakPtr()));
}

void NexusShellServiceImpl::OnResolved(ResolvedCapabilities capabilities) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  cached_capabilities_ = std::move(capabilities);
  resolve_in_flight_ = false;

  std::vector<GetCapabilitiesCallback> drained_caps;
  drained_caps.swap(pending_callbacks_);
  for (auto& cb : drained_caps) {
    RespondFromCache(std::move(cb));
  }

  std::vector<PendingExec> drained_execs;
  drained_execs.swap(pending_execs_);
  for (auto& pending : drained_execs) {
    RunExec(std::move(pending.options), std::move(pending.callback));
  }

  std::vector<PendingSpawn> drained_spawns;
  drained_spawns.swap(pending_spawns_);
  for (auto& pending : drained_spawns) {
    RunSpawn(std::move(pending.options), std::move(pending.callback));
  }

  std::vector<PendingStreamSpawn> drained_stream_spawns;
  drained_stream_spawns.swap(pending_stream_spawns_);
  for (auto& pending : drained_stream_spawns) {
    RunStreamSpawn(std::move(pending.options), std::move(pending.controller),
                   std::move(pending.client), std::move(pending.callback));
  }
}

void NexusShellServiceImpl::RespondFromCache(
    GetCapabilitiesCallback callback) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  CHECK(cached_capabilities_.has_value());
  const ResolvedCapabilities& caps = *cached_capabilities_;

  mojom::ShellResolvedToolPtr default_shell;
  if (caps.default_shell.has_value()) {
    default_shell = ToMojom(*caps.default_shell);
  }

  std::vector<mojom::ShellResolvedToolPtr> tools;
  tools.reserve(caps.tools.size());
  for (const ResolvedTool& tool : caps.tools) {
    tools.push_back(ToMojom(tool));
  }

  std::move(callback).Run(caps.platform, std::move(default_shell),
                          std::move(tools), caps.command_allowlist);
}

void NexusShellServiceImpl::RunExec(mojom::ShellSpawnOptionsPtr options,
                                    ExecCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  CHECK(cached_capabilities_.has_value());

  // Validate + resolve into a runnable request (shared with Spawn).
  PreparedExec prep = PrepareExec(*options, *cached_capabilities_);
  if (!prep.ok) {
    // Surface the install hint when the missing-binary case fires; for
    // other error codes the hint is empty and the helper omits it.
    RunErrorReply(std::move(callback), prep.error_code, prep.error_message,
                  prep.install_hint);
    return;
  }
  ChildProcessRequest request = std::move(prep.request);

  // Auto-background-on-timeout (CC parity): route a slow foreground command
  // to the timer-race path so crossing the caller's budget DETACHES the
  // still-running child instead of killing it. See
  // StartExecWithBackgroundFallback.
  if (options->background_on_timeout) {
    StartExecWithBackgroundFallback(std::move(request), std::move(callback));
    return;
  }

  const base::TimeTicks dispatch_start = base::TimeTicks::Now();

  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      // WithBaseSyncPrimitives() is required because RunChildProcess
      // (via child_process_runner.cc) calls
      // base::Process::WaitForExitWithTimeout — Chromium classifies the
      // child-process wait as a "base sync primitive" and DCHECKs at
      // base/threading/thread_restrictions.cc:166 if the task wasn't
      // declared to allow it. Without this trait, every Bash invocation
      // crashes the utility process on first wait.
      {base::MayBlock(), base::WithBaseSyncPrimitives(),
       base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      // Foreground Exec is never killed out-of-band — no kill handle needed.
      base::BindOnce(&RunChildProcess, std::move(request),
                     base::OnceCallback<void(ChildProcessKillHandle)>()),
      base::BindOnce(
          [](ExecCallback callback, base::TimeTicks dispatch_start,
             ChildProcessOutcome outcome) {
            if (!outcome.launched) {
              // Launch failures past the policy check are infrastructure
              // problems (couldn't open temp files, OS rejected fork);
              // there is no install hint we can offer.
              std::move(callback).Run(/*ok=*/false, /*result=*/nullptr,
                                      outcome.launch_error_code,
                                      outcome.launch_error_message,
                                      /*install_hint=*/std::nullopt);
              return;
            }

            // Total wall-clock from the time the binding sequence
            // dispatched the work — includes thread-pool queueing,
            // not just child runtime. Surfaces queueing pressure to
            // the LLM ("why is this slow?") without needing per-call
            // tracing.
            const base::TimeDelta total =
                base::TimeTicks::Now() - dispatch_start;
            std::move(callback).Run(/*ok=*/true,
                                    ToMojomExecResult(outcome, total),
                                    /*error_code=*/std::nullopt,
                                    /*error_message=*/std::nullopt,
                                    /*install_hint=*/std::nullopt);
          },
          std::move(callback), dispatch_start));
}

void NexusShellServiceImpl::Spawn(mojom::ShellSpawnOptionsPtr options,
                                  SpawnCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (!options) {
    std::move(callback).Run(/*ok=*/false, /*shell_id=*/std::nullopt,
                            "INVALID_ARGS",
                            "Spawn requires a non-null ShellSpawnOptions",
                            /*install_hint=*/std::nullopt);
    return;
  }

  // Queue until the first capability probe finishes (mirrors Exec).
  if (!cached_capabilities_.has_value()) {
    PendingSpawn pending;
    pending.options = std::move(options);
    pending.callback = std::move(callback);
    pending_spawns_.push_back(std::move(pending));
    EnsureResolveStarted();
    return;
  }

  RunSpawn(std::move(options), std::move(callback));
}

void NexusShellServiceImpl::RunSpawn(mojom::ShellSpawnOptionsPtr options,
                                     SpawnCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  CHECK(cached_capabilities_.has_value());

  // Same validation / allowlist / cwd / shell-rewrite as the foreground
  // path. Pre-launch rejections reply with error_* and no shell_id.
  PreparedExec prep = PrepareExec(*options, *cached_capabilities_);
  if (!prep.ok) {
    std::optional<std::string> hint;
    if (!prep.install_hint.empty()) {
      hint = prep.install_hint;
    }
    std::move(callback).Run(/*ok=*/false, /*shell_id=*/std::nullopt,
                            prep.error_code, prep.error_message, hint);
    return;
  }

  // Bound memory: evict the oldest COMPLETED entry when at capacity.
  // Running entries are never evicted (their outcome is still pending).
  if (background_.size() >= kMaxBackgroundEntries) {
    for (auto it = background_.begin(); it != background_.end(); ++it) {
      if (!it->second.running) {
        background_.erase(it);
        break;
      }
    }
  }

  // Allocate a handle + tracking entry, then reply IMMEDIATELY — the
  // process runs to completion on a worker while the caller's turn
  // continues. The model polls via ReadBackgroundOutput(shell_id).
  std::string shell_id =
      base::StrCat({"bg_", base::Uuid::GenerateRandomV4().AsLowercaseString()});
  background_[shell_id] = BackgroundEntry{};

  std::move(callback).Run(/*ok=*/true, shell_id, /*error_code=*/std::nullopt,
                          /*error_message=*/std::nullopt,
                          /*install_hint=*/std::nullopt);

  const base::TimeTicks dispatch_start = base::TimeTicks::Now();
  // Capture the live child's tree-kill handle at launch — bounced back to this
  // binding sequence — so `Kill(shell_id)` can terminate the whole descendant
  // tree while it runs.
  scoped_refptr<base::SequencedTaskRunner> binding_runner =
      base::SequencedTaskRunner::GetCurrentDefault();
  auto on_launched = base::BindPostTask(
      binding_runner,
      base::BindOnce(&NexusShellServiceImpl::OnBackgroundLaunched,
                     weak_factory_.GetWeakPtr(), shell_id));
  auto on_output = base::BindPostTask(
      binding_runner,
      base::BindRepeating(&NexusShellServiceImpl::OnBackgroundOutput,
                          weak_factory_.GetWeakPtr(), shell_id));
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      // WithBaseSyncPrimitives(): RunChildProcess waits on the child —
      // same trait requirement as the Exec path.
      {base::MayBlock(), base::WithBaseSyncPrimitives(),
       base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(&RunChildProcessWithOutput, std::move(prep.request),
                     std::move(on_launched), std::move(on_output)),
      base::BindOnce(&NexusShellServiceImpl::OnBackgroundComplete,
                     weak_factory_.GetWeakPtr(), shell_id, dispatch_start));
}

void NexusShellServiceImpl::OnBackgroundComplete(
    std::string shell_id,
    base::TimeTicks dispatch_start,
    ChildProcessOutcome outcome) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = background_.find(shell_id);
  if (it == background_.end()) {
    // Entry was evicted while the process ran — nothing to record.
    return;
  }
  it->second.running = false;
  it->second.duration = base::TimeTicks::Now() - dispatch_start;
  // If this child died because `Kill` actually terminated it, the runner may
  // observe a "normal" exit — force the killed flag so ReadBackgroundOutput
  // reports `exit.killed = true`.
  if (it->second.kill_issued) {
    outcome.killed = true;
    outcome.timed_out = false;
  }
  it->second.stdout_so_far = outcome.stdout_bytes;
  it->second.stderr_so_far = outcome.stderr_bytes;
  it->second.stdout_truncated_so_far = outcome.stdout_truncated;
  it->second.stderr_truncated_so_far = outcome.stderr_truncated;
  it->second.outcome = std::move(outcome);
  it->second.kill_handle = ChildProcessKillHandle();  // release tree-kill handle
}

void NexusShellServiceImpl::OnBackgroundLaunched(
    std::string shell_id,
    ChildProcessKillHandle kill_handle) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = background_.find(shell_id);
  if (it == background_.end() || !it->second.running) {
    // Evicted, or the child already completed before the handle arrived —
    // the handle is no longer useful.
    return;
  }
  it->second.launch_observed = true;
  if (it->second.kill_requested) {
    // `Kill` raced ahead of launch — terminate now (it couldn't earlier
    // because no tree handle was stored yet).
    if (TerminateBackgroundTree(kill_handle)) {
      it->second.kill_issued = true;
    }
    return;
  }
  it->second.kill_handle = std::move(kill_handle);
}

void NexusShellServiceImpl::OnBackgroundOutput(std::string shell_id,
                                               bool is_stdout,
                                               std::vector<uint8_t> bytes,
                                               bool truncated) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = background_.find(shell_id);
  if (it == background_.end() || !it->second.running) {
    return;
  }
  if (is_stdout) {
    it->second.stdout_so_far = std::move(bytes);
    it->second.stdout_truncated_so_far = truncated;
  } else {
    it->second.stderr_so_far = std::move(bytes);
    it->second.stderr_truncated_so_far = truncated;
  }
}

void NexusShellServiceImpl::Kill(const std::string& shell_id,
                                 KillCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = background_.find(shell_id);
  if (it == background_.end()) {
    std::move(callback).Run(/*found=*/false, /*was_running=*/false);
    return;
  }
  if (!it->second.running) {
    // Already terminal — idempotent no-op.
    std::move(callback).Run(/*found=*/true, /*was_running=*/false);
    return;
  }
  // Mark first so a handle that lands AFTER this (the launch/Kill race) is
  // terminated by OnBackgroundLaunched. OnBackgroundComplete forces the killed
  // flag only if tree termination was actually issued.
  it->second.kill_requested = true;
  if (it->second.launch_observed && !it->second.kill_handle.IsValid()) {
    std::move(callback).Run(/*found=*/true, /*was_running=*/false);
    return;
  }
  if (it->second.kill_handle.IsValid()) {
    // Best-effort, non-blocking tree termination; the worker's WaitForExit
    // observes the death and OnBackgroundComplete records the forced-killed
    // outcome. The temp-file output captured up to the kill is still read back
    // there.
    if (!TerminateBackgroundTree(it->second.kill_handle)) {
      std::move(callback).Run(/*found=*/true, /*was_running=*/false);
      return;
    }
    it->second.kill_issued = true;
  }
  std::move(callback).Run(/*found=*/true, /*was_running=*/true);
}

void NexusShellServiceImpl::StartExecWithBackgroundFallback(
    ChildProcessRequest request,
    ExecCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // `request.timeout` is the caller's clamped foreground budget (set by
  // PrepareExec from options.timeout_ms). Repurpose it as the BLOCK BUDGET
  // for the timer, and run the child under the hard server cap so crossing
  // the budget detaches rather than kills.
  const base::TimeDelta block_budget = request.timeout;
  request.timeout = base::Milliseconds(SafeCommandLimits::kMaxTimeoutMs);

  // Bound memory: evict the oldest COMPLETED background entry at capacity
  // (mirrors RunSpawn). Running entries are never evicted.
  if (background_.size() >= kMaxBackgroundEntries) {
    for (auto it = background_.begin(); it != background_.end(); ++it) {
      if (!it->second.running) {
        background_.erase(it);
        break;
      }
    }
  }

  // Reserve the handle up front so the timer and the completion callback
  // key off the same id.
  std::string shell_id =
      base::StrCat({"bg_", base::Uuid::GenerateRandomV4().AsLowercaseString()});
  background_[shell_id] = BackgroundEntry{};

  auto state = std::make_unique<ExecBackgroundState>();
  state->callback = std::move(callback);
  state->dispatch_start = base::TimeTicks::Now();
  state->timer = std::make_unique<base::OneShotTimer>();
  state->timer->Start(
      FROM_HERE, block_budget,
      base::BindOnce(&NexusShellServiceImpl::OnExecBackgroundTimeout,
                     weak_factory_.GetWeakPtr(), shell_id));
  exec_background_[shell_id] = std::move(state);

  // Same launch tree-kill handle capture as RunSpawn, so a command
  // auto-detached on timeout is also killable via `Kill(shell_id)`.
  scoped_refptr<base::SequencedTaskRunner> binding_runner =
      base::SequencedTaskRunner::GetCurrentDefault();
  auto on_launched = base::BindPostTask(
      binding_runner,
      base::BindOnce(&NexusShellServiceImpl::OnBackgroundLaunched,
                     weak_factory_.GetWeakPtr(), shell_id));
  auto on_output = base::BindPostTask(
      binding_runner,
      base::BindRepeating(&NexusShellServiceImpl::OnBackgroundOutput,
                          weak_factory_.GetWeakPtr(), shell_id));
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE,
      // WithBaseSyncPrimitives(): RunChildProcess waits on the child —
      // same trait requirement as the Exec / Spawn paths.
      {base::MayBlock(), base::WithBaseSyncPrimitives(),
       base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
      base::BindOnce(&RunChildProcessWithOutput, std::move(request),
                     std::move(on_launched), std::move(on_output)),
      base::BindOnce(&NexusShellServiceImpl::OnExecBackgroundComplete,
                     weak_factory_.GetWeakPtr(), shell_id));
}

void NexusShellServiceImpl::OnExecBackgroundTimeout(std::string shell_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = exec_background_.find(shell_id);
  if (it == exec_background_.end()) {
    return;  // The worker already completed and erased the state.
  }
  ExecBackgroundState* state = it->second.get();
  if (state->replied) {
    return;  // Defensive — the completion path already replied.
  }
  state->replied = true;

  // Hand the still-running child to the background registry: it keeps
  // running under the backstop timeout and the caller polls
  // ReadBackgroundOutput(shell_id). NOT killed — exit.killed = false.
  // Mirrors Claude Code's tengu_bash_command_timeout_backgrounded.
  auto result = mojom::ShellExecResult::New();
  auto exit = mojom::ShellExitInfo::New();
  exit->timed_out = true;
  exit->killed = false;
  exit->duration_ms = static_cast<uint64_t>(
      (base::TimeTicks::Now() - state->dispatch_start).InMilliseconds());
  result->exit = std::move(exit);
  result->stdout_buffer = mojo_base::BigBuffer();
  result->stderr_buffer = mojo_base::BigBuffer();
  result->background_shell_id = shell_id;

  std::move(state->callback)
      .Run(/*ok=*/true, std::move(result), /*error_code=*/std::nullopt,
           /*error_message=*/std::nullopt, /*install_hint=*/std::nullopt);
  // Keep the exec_background_ entry until OnExecBackgroundComplete records
  // the final outcome into background_; the timer has fired and is inert.
}

void NexusShellServiceImpl::OnExecBackgroundComplete(
    std::string shell_id,
    ChildProcessOutcome outcome) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = exec_background_.find(shell_id);
  if (it == exec_background_.end()) {
    // State already gone — still record into background_ if it survives.
    auto bg = background_.find(shell_id);
    if (bg != background_.end()) {
      if (bg->second.kill_issued) {
        outcome.killed = true;
        outcome.timed_out = false;
      }
      bg->second.running = false;
      bg->second.stdout_so_far = outcome.stdout_bytes;
      bg->second.stderr_so_far = outcome.stderr_bytes;
      bg->second.stdout_truncated_so_far = outcome.stdout_truncated;
      bg->second.stderr_truncated_so_far = outcome.stderr_truncated;
      bg->second.outcome = std::move(outcome);
    }
    return;
  }

  if (it->second->replied) {
    // Timed out earlier → already surfaced to the model as a background
    // command. Record the final outcome so ReadBackgroundOutput returns it.
    const base::TimeTicks dispatch_start = it->second->dispatch_start;
    auto bg = background_.find(shell_id);
    if (bg != background_.end()) {
      if (bg->second.kill_issued) {
        outcome.killed = true;
        outcome.timed_out = false;
      }
      bg->second.running = false;
      bg->second.duration = base::TimeTicks::Now() - dispatch_start;
      bg->second.stdout_so_far = outcome.stdout_bytes;
      bg->second.stderr_so_far = outcome.stderr_bytes;
      bg->second.stdout_truncated_so_far = outcome.stdout_truncated;
      bg->second.stderr_truncated_so_far = outcome.stderr_truncated;
      bg->second.outcome = std::move(outcome);
    }
    exec_background_.erase(it);
    return;
  }

  // Completed within the block budget → reply as a normal foreground Exec
  // and drop the reserved background slot (the model never saw its id).
  it->second->timer->Stop();
  ExecCallback callback = std::move(it->second->callback);
  const base::TimeDelta total =
      base::TimeTicks::Now() - it->second->dispatch_start;
  background_.erase(shell_id);
  exec_background_.erase(it);  // `it` / state invalid past this point.

  if (!outcome.launched) {
    std::move(callback).Run(/*ok=*/false, /*result=*/nullptr,
                            outcome.launch_error_code,
                            outcome.launch_error_message,
                            /*install_hint=*/std::nullopt);
    return;
  }
  std::move(callback).Run(/*ok=*/true, ToMojomExecResult(outcome, total),
                          /*error_code=*/std::nullopt,
                          /*error_message=*/std::nullopt,
                          /*install_hint=*/std::nullopt);
}

void NexusShellServiceImpl::ReadBackgroundOutput(
    const std::string& shell_id,
    ReadBackgroundOutputCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  auto out = mojom::ShellBackgroundOutput::New();
  auto it = background_.find(shell_id);
  if (it == background_.end()) {
    out->found = false;
    out->running = false;
    out->stdout_buffer = mojo_base::BigBuffer();
    out->stderr_buffer = mojo_base::BigBuffer();
    std::move(callback).Run(std::move(out));
    return;
  }

  out->found = true;
  out->running = it->second.running;

  if (it->second.running || !it->second.outcome.has_value()) {
    // Still executing — return the latest accumulated snapshots collected by
    // the runner from the temp files. Exit stays null until completion.
    out->stdout_buffer = BytesToBigBuffer(it->second.stdout_so_far);
    out->stderr_buffer = BytesToBigBuffer(it->second.stderr_so_far);
    out->stdout_truncated = it->second.stdout_truncated_so_far;
    out->stderr_truncated = it->second.stderr_truncated_so_far;
    std::move(callback).Run(std::move(out));
    return;
  }

  const ChildProcessOutcome& o = *it->second.outcome;
  auto exit = mojom::ShellExitInfo::New();
  exit->duration_ms =
      static_cast<uint64_t>(it->second.duration.InMilliseconds());

  if (!o.launched) {
    // Launch failed after Spawn already returned the shell_id — surface the infra
    // error through stderr + a non-zero exit so the JS poller delivers it as a
    // background-shell failure. `killed` is reserved for explicit Stop/timeout.
    std::string msg =
        base::StrCat({o.launch_error_code, ": ", o.launch_error_message});
    std::vector<uint8_t> bytes(msg.begin(), msg.end());
    out->stderr_buffer = BytesToBigBuffer(bytes);
    out->stdout_buffer = mojo_base::BigBuffer();
    exit->code = -1;
  } else {
    out->stdout_buffer = BytesToBigBuffer(o.stdout_bytes);
    out->stderr_buffer = BytesToBigBuffer(o.stderr_bytes);
    out->stdout_truncated = o.stdout_truncated;
    out->stderr_truncated = o.stderr_truncated;
    exit->code = o.code;
    exit->signal = o.signal;
    exit->timed_out = o.timed_out;
    exit->killed = o.killed;
  }
  out->exit = std::move(exit);
  std::move(callback).Run(std::move(out));
}

void NexusShellServiceImpl::StreamSpawn(
    mojom::ShellSpawnOptionsPtr options,
    mojo::PendingReceiver<mojom::NexusStreamingProcess> controller,
    mojo::PendingRemote<mojom::NexusStreamingProcessClient> client,
    StreamSpawnCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (!options) {
    std::move(callback).Run(/*ok=*/false, "INVALID_ARGS",
                            "StreamSpawn requires a non-null ShellSpawnOptions",
                            /*install_hint=*/std::nullopt);
    return;
  }

  // Queue until the first capability probe finishes (mirrors Exec/Spawn).
  if (!cached_capabilities_.has_value()) {
    PendingStreamSpawn pending;
    pending.options = std::move(options);
    pending.controller = std::move(controller);
    pending.client = std::move(client);
    pending.callback = std::move(callback);
    pending_stream_spawns_.push_back(std::move(pending));
    EnsureResolveStarted();
    return;
  }

  RunStreamSpawn(std::move(options), std::move(controller), std::move(client),
                 std::move(callback));
}

void NexusShellServiceImpl::RunStreamSpawn(
    mojom::ShellSpawnOptionsPtr options,
    mojo::PendingReceiver<mojom::NexusStreamingProcess> controller,
    mojo::PendingRemote<mojom::NexusStreamingProcessClient> client,
    StreamSpawnCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  CHECK(cached_capabilities_.has_value());

  // Identical allowlist / cwd / shell-rewrite / env policy as Exec/Spawn.
  // `stdin_data` / `timeout_ms` in the request are ignored for streaming
  // (StreamingChildProcess uses live pipes + controller-driven lifetime).
  PreparedExec prep = PrepareExec(*options, *cached_capabilities_);
  if (!prep.ok) {
    std::optional<std::string> hint;
    if (!prep.install_hint.empty()) {
      hint = prep.install_hint;
    }
    std::move(callback).Run(/*ok=*/false, prep.error_code, prep.error_message,
                            hint);
    return;
  }

  const uint64_t id = next_streaming_id_++;
  auto child = std::make_unique<StreamingChildProcess>(
      std::move(prep.request), std::move(controller), std::move(client),
      base::BindOnce(&NexusShellServiceImpl::OnStreamingChildClosed,
                     weak_factory_.GetWeakPtr(), id));
  StreamingChildProcess* child_ptr = child.get();
  streaming_children_[id] = std::move(child);

  // Reply BEFORE Start() so the browser can bind its controller Remote and
  // begin pipelining WriteStdin (queued internally until launch finishes).
  std::move(callback).Run(/*ok=*/true, /*error_code=*/std::nullopt,
                          /*error_message=*/std::nullopt,
                          /*install_hint=*/std::nullopt);
  child_ptr->Start();
}

void NexusShellServiceImpl::OnStreamingChildClosed(uint64_t id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // The child fired its own OnExit before invoking this; we erase the
  // unique_ptr here. Post the erase so we never destroy the object from
  // within its own synchronous call frame.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<NexusShellServiceImpl> self, uint64_t id) {
            if (self) {
              self->streaming_children_.erase(id);
            }
          },
          weak_factory_.GetWeakPtr(), id));
}

}  // namespace nexus
