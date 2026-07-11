// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/process/nexus_process_bridge_adapter.h"

#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "custom_browser/browser/nexus/nexus_bridge_support.h"
#include "mojo/public/cpp/base/big_buffer.h"

namespace nexus {

namespace {

using base::Value;
using Dict = base::DictValue;
using List = base::ListValue;

Value ToolToValue(const mojom::ShellResolvedToolPtr& tool) {
  Dict out;
  out.Set("id", tool->id);
  out.Set("path", tool->path.AsUTF8Unsafe());
  out.Set("version", tool->version);
  out.Set("available", tool->available);
  return Value(std::move(out));
}

Value ExitInfoToValue(const mojom::ShellExitInfoPtr& exit) {
  Dict out;
  if (exit->code.has_value()) {
    out.Set("code", *exit->code);
  } else {
    out.Set("code", Value());  // null
  }
  if (exit->signal.has_value()) {
    out.Set("signal", *exit->signal);
  } else {
    out.Set("signal", Value());  // null
  }
  out.Set("timedOut", exit->timed_out);
  out.Set("killed", exit->killed);
  out.Set("durationMs", static_cast<double>(exit->duration_ms));
  return Value(std::move(out));
}

}  // namespace

NexusProcessBridgeAdapter::NexusProcessBridgeAdapter(
    NexusBridgeResponseDelegate* delegate,
    ServiceFactory service_factory)
    : NexusBridgeCapabilityHandler(delegate),
      service_factory_(std::move(service_factory)) {
  Register("getCapabilities",
           &NexusProcessBridgeAdapter::HandleGetCapabilities);
  Register("exec", &NexusProcessBridgeAdapter::HandleExec);
}

NexusProcessBridgeAdapter::~NexusProcessBridgeAdapter() = default;

void NexusProcessBridgeAdapter::HandleGetCapabilities(
    const std::string& id,
    const Dict& route,
    const Dict& /*payload*/,
    std::vector<blink::MessagePortDescriptor> /*ports*/) {
  mojo::Remote<mojom::NexusShellService>& remote = service_factory_.Run();
  if (!remote.is_bound()) {
    SendErrorResponse(id, route, "PROCESS_SERVICE_UNAVAILABLE",
                      "NexusShellService remote is not bound");
    return;
  }

  remote->GetCapabilities(base::BindOnce(
      [](base::WeakPtr<NexusProcessBridgeAdapter> self, std::string id,
         Dict route, const std::string& platform,
         mojom::ShellResolvedToolPtr default_shell,
         std::vector<mojom::ShellResolvedToolPtr> tools,
         const std::vector<std::string>& allowlist) {
        if (!self) {
          return;
        }
        Dict result;
        result.Set("platform", platform);
        if (default_shell) {
          result.Set("defaultShell", ToolToValue(default_shell));
        } else {
          result.Set("defaultShell", Value());  // null
        }
        List tools_list;
        for (const auto& tool : tools) {
          tools_list.Append(ToolToValue(tool));
        }
        result.Set("tools", Value(std::move(tools_list)));
        List allowlist_list;
        for (const std::string& id_entry : allowlist) {
          allowlist_list.Append(id_entry);
        }
        result.Set("commandAllowlist", Value(std::move(allowlist_list)));
        self->SendSuccessResponse(id, route, Value(std::move(result)));
      },
      weak_factory_.GetWeakPtr(), id, route.Clone()));
}

mojom::ShellSpawnOptionsPtr NexusProcessBridgeAdapter::BuildSpawnOptions(
    const Dict& payload,
    std::string* error) const {
  auto options = mojom::ShellSpawnOptions::New();

  std::string command;
  if (!bridge_support::CopyRequiredString(payload, "command", &command,
                                          error)) {
    return nullptr;
  }
  options->command = std::move(command);

  if (const List* args = payload.FindList("args")) {
    options->args.reserve(args->size());
    for (const Value& entry : *args) {
      if (!entry.is_string()) {
        *error = "args[] entries must be strings";
        return nullptr;
      }
      options->args.push_back(entry.GetString());
    }
  }

  std::string cwd_str;
  if (!bridge_support::CopyRequiredString(payload, "cwd", &cwd_str, error)) {
    return nullptr;
  }
  options->cwd = base::FilePath::FromUTF8Unsafe(cwd_str);

  if (const Dict* env = payload.FindDict("env")) {
    options->env.reserve(env->size());
    for (const auto entry : *env) {
      if (!entry.second.is_string()) {
        *error = "env values must be strings";
        return nullptr;
      }
      auto kv = mojom::ShellEnvEntry::New();
      kv->name = entry.first;
      kv->value = entry.second.GetString();
      options->env.push_back(std::move(kv));
    }
  }

  options->use_shell = payload.FindBool("useShell").value_or(false);
  options->timeout_ms = base::checked_cast<uint32_t>(
      std::max<int64_t>(0, bridge_support::FindIntegralValue(payload,
                                                              "timeoutMs")
                              .value_or(0)));
  options->max_output_bytes = base::checked_cast<uint32_t>(
      std::max<int64_t>(0, bridge_support::FindIntegralValue(payload,
                                                              "maxOutputBytes")
                              .value_or(0)));
  return options;
}

void NexusProcessBridgeAdapter::HandleExec(
    const std::string& id,
    const Dict& route,
    const Dict& payload,
    std::vector<blink::MessagePortDescriptor> /*ports*/) {
  std::string error;
  mojom::ShellSpawnOptionsPtr options = BuildSpawnOptions(payload, &error);
  if (!options) {
    SendInvalidArgsResponse(id, route, error);
    return;
  }

  mojo::Remote<mojom::NexusShellService>& remote = service_factory_.Run();
  if (!remote.is_bound()) {
    SendErrorResponse(id, route, "PROCESS_SERVICE_UNAVAILABLE",
                      "NexusShellService remote is not bound");
    return;
  }

  remote->Exec(
      std::move(options),
      base::BindOnce(
          [](base::WeakPtr<NexusProcessBridgeAdapter> self, std::string id,
             Dict route, bool ok, mojom::ShellExecResultPtr result,
             const std::optional<std::string>& error_code,
             const std::optional<std::string>& error_message,
             const std::optional<std::string>& install_hint) {
            if (!self) {
              return;
            }
            if (!ok || !result) {
              // Pre-launch failures are surfaced as a *successful* bridge
              // response whose payload is the `ProcessExecFailure`
              // shape (`{ ok: false, errorCode, errorMessage,
              // installHint? }`). The guest BrowserHostBridge.process.exec()
              // contract requires the promise to resolve with the union;
              // throwing here would lose `installHint` and force the
              // guest to invent a synthetic failure object.
              Dict failure;
              failure.Set("ok", false);
              failure.Set(
                  "errorCode",
                  error_code.value_or(std::string("PROCESS_EXEC_FAILED")));
              failure.Set(
                  "errorMessage",
                  error_message.value_or(std::string("Exec failed")));
              if (install_hint.has_value() && !install_hint->empty()) {
                failure.Set("installHint", *install_hint);
              }
              self->SendSuccessResponse(id, route,
                                        Value(std::move(failure)));
              return;
            }

            // Pack the result into a JSON object plus *two* binary
            // sidecars (stdout, stderr). The existing
            // `SendBinaryResponse` only carries one binary stream, so
            // we fall back to inlining the bytes as base64-free byte
            // arrays only when both streams are small. Otherwise we
            // pack `stdout` via the binary side-channel and inline
            // `stderr` (which is typically much smaller).
            //
            // Slice 3's guest protocol mirrors this layout: when
            // `result.stdoutBinary === true`, `stdout` arrives via the
            // transferred MessagePort under the field name "$stdout";
            // `stderr` is always inline.
            Dict result_dict;
            // Discriminator field expected by ProcessExecResult (the
            // failure branch sets `ok: false`; success must set `ok: true`
            // so guest consumers can switch on the union without having
            // to probe for `errorCode`).
            result_dict.Set("ok", true);
            result_dict.Set("exit", ExitInfoToValue(result->exit));
            result_dict.Set("stdoutTruncated", result->stdout_truncated);
            result_dict.Set("stderrTruncated", result->stderr_truncated);

            // stderr is inlined as a number array — small in practice,
            // and avoids needing two transferred ports per response.
            const base::span<const uint8_t> stderr_span =
                base::span<const uint8_t>(result->stderr_buffer);
            List stderr_list;
            stderr_list.reserve(stderr_span.size());
            for (uint8_t b : stderr_span) {
              stderr_list.Append(static_cast<int>(b));
            }
            result_dict.Set("stderr", Value(std::move(stderr_list)));

            self->SendBinaryResponse(id, route, Value(std::move(result_dict)),
                                     std::move(result->stdout_buffer),
                                     "stdout");
          },
          weak_factory_.GetWeakPtr(), id, route.Clone()));
}

}  // namespace nexus
