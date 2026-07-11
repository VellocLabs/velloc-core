// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/utility/nexus/shell/nexus_shell_service_impl.h"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "custom_browser/browser/nexus/mojom/nexus_shell.mojom.h"
#include "custom_browser/utility/nexus/shell/child_process_runner.h"
#include "custom_browser/utility/nexus/shell/safe_command_policy.h"
#include "mojo/public/cpp/base/big_buffer.h"

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

}  // namespace

NexusShellServiceImpl::PendingExec::PendingExec() = default;

NexusShellServiceImpl::PendingExec::~PendingExec() = default;

NexusShellServiceImpl::PendingExec::PendingExec(PendingExec&&) = default;

NexusShellServiceImpl::PendingExec&
NexusShellServiceImpl::PendingExec::operator=(PendingExec&&) = default;

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
  const ResolvedCapabilities& caps = *cached_capabilities_;

  // ----- Step 1: validate cwd. We do this *before* command resolution
  // so the model gets the most-specific error first if both are wrong.
  std::string cwd_reason;
  if (!SafeCommandPolicy::ValidateCwd(options->cwd, &cwd_reason)) {
    RunErrorReply(std::move(callback), "INVALID_CWD", cwd_reason);
    return;
  }

  // ----- Step 2: resolve command (allowlist + default-shell rewrite).
  // When `use_shell` is true, the resolved exe is the host's default
  // shell; the model-supplied `command` becomes the script body that we
  // hand to the shell as -c / /C. When false, we resolve `command`
  // against the allowlist directly.
  std::string final_command = options->command;
  std::vector<std::string> final_args = options->args;

  if (options->use_shell) {
    if (!caps.default_shell.has_value()) {
      RunErrorReply(std::move(callback), "NO_DEFAULT_SHELL",
                    "Host has no resolvable default shell on PATH");
      return;
    }
    const ResolvedTool& shell = *caps.default_shell;

    // Build the shell argv: [<shell>, "-c"|"/C", <script>, <extra_args...>]
    // Extra args (options->args) are passed after the script string and
    // become $1..$N inside the script per POSIX `sh -c "script" name arg1`
    // convention. On Windows cmd we ignore extras (cmd /C does not have
    // an argv-passing convention).
    std::vector<std::string> shell_args;
    if (shell.id == "cmd") {
      shell_args.push_back("/C");
      shell_args.push_back(options->command);
      // Intentionally drop options->args on cmd.exe — see comment above.
    } else {
      shell_args.push_back("-c");
      shell_args.push_back(options->command);
      // POSIX convention: argv[0] of the script is the shell name itself.
      shell_args.push_back(shell.id);
      for (const std::string& arg : options->args) {
        shell_args.push_back(arg);
      }
    }

    final_command = shell.id;  // policy lookup below uses this id
    final_args = std::move(shell_args);
  }

  PolicyDecision decision =
      SafeCommandPolicy::ResolveCommand(final_command, caps);
  if (!decision.ok()) {
    // Surface the install hint when the missing-binary case fires; for
    // other error codes the hint is empty and the helper omits it from
    // the Mojo reply.
    RunErrorReply(std::move(callback), decision.error_code,
                  decision.error_message, decision.install_hint);
    return;
  }

  // ----- Step 3: build the request and post to the thread pool.
  ChildProcessRequest request;
  request.exe = decision.resolved_path;
  request.args = std::move(final_args);
  request.cwd = options->cwd;
  request.stdin_data = options->stdin_data;

  std::vector<EnvOverride> env_overrides;
  env_overrides.reserve(options->env.size());
  for (const auto& entry : options->env) {
    env_overrides.push_back({entry->name, entry->value});
  }
  request.env = SafeCommandPolicy::BuildSafeEnvironment(env_overrides);

  request.timeout = base::Milliseconds(
      SafeCommandPolicy::ClampTimeoutMs(options->timeout_ms));
  request.max_output_bytes =
      SafeCommandPolicy::ClampMaxOutputBytes(options->max_output_bytes);

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
      base::BindOnce(&RunChildProcess, std::move(request)),
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

}  // namespace nexus
