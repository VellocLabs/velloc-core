// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_SHELL_NEXUS_SHELL_BRIDGE_ADAPTER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_SHELL_NEXUS_SHELL_BRIDGE_ADAPTER_H_

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "custom_browser/browser/nexus/api/shell/shell_api.gen.h"
#include "custom_browser/browser/nexus/bridge_response_callback.h"
#include "custom_browser/browser/nexus/mojom/nexus_shell.mojom.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"

class Profile;

namespace blink {
class MessagePortDescriptor;
}

namespace nexus {

class NexusBridgeResponseDelegate;

// Concrete adapter implementing the generated `ShellApiBase` virtuals — the
// guest-side observe/control surface for BACKGROUND shells (those the model
// starts via the Bash tool's `run_in_background` mode → `ProcessTool::Spawn`).
//
//   shell.status(shellId) → NexusShellService::ReadBackgroundOutput
//   shell.kill(shellId)   → NexusShellService::Kill
//
// and — for the plugin `$.process.spawn` noun (F-25 H-13) — the STREAMING
// surface over `NexusShellService::StreamSpawn`, the live-pipe path the
// utility service already has:
//
//   shell.spawn(shell, command, …) → StreamSpawn; { streamId }
//   shell.writeStdin / closeStdin / streamKill(streamId) → the controller
//   events shell.onStreamOutput { streamId, stream, dataBase64 } and
//          shell.onStreamExit { streamId, exitCode, … }
//
// A streaming child is NOT in the background-shell registry (no shell_id,
// nothing for status/kill); its life is the adapter's: torn down with it.
// Scope stays a guest-side observe/control API like fs.* / model.*, not a
// revival of the removed Phase-12g `process` capability.
//
// KEY INVARIANT: this adapter and `ProcessTool` BOTH funnel through the
// per-Profile shared `NexusShellServiceLauncher` (`GetSharedShellLauncher`), so
// they share ONE utility process and thus ONE background-shell registry — the
// guest can therefore observe/kill the very shells the model spawned. With a
// separate launcher the adapter would connect to a different utility process
// whose registry can't see them (it would compile but never function).
//
// Both actions run on UI (the *.api.json declare runtime.thread = "ui"): each
// fires an ASYNC mojo call on the shared launcher's remote and replies from the
// mojo reply callback (which also lands on UI). No blocking work, so no thread
// hop. The reply shapers are free functions binding only `done` (never `this`),
// so a port-service teardown racing an in-flight call can't use-after-free the
// adapter — `done` weak-guards the handler and drops cleanly.
class NexusShellBridgeAdapter : public ::nexus::generated::ShellApiBase {
 public:
  NexusShellBridgeAdapter(NexusBridgeResponseDelegate* delegate,
                          Profile* profile);
  ~NexusShellBridgeAdapter() override;

  NexusShellBridgeAdapter(const NexusShellBridgeAdapter&) = delete;
  NexusShellBridgeAdapter& operator=(const NexusShellBridgeAdapter&) = delete;

  // Most names one shell.env call may read.
  static constexpr size_t kMaxEnvNames = 64;

 private:
  void HandleStatusOnUI(
      ::nexus::generated::StatusInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleKillOnUI(
      ::nexus::generated::KillInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleSpawnOnUI(
      ::nexus::generated::SpawnInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleWriteStdinOnUI(
      ::nexus::generated::WriteStdinInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleCloseStdinOnUI(
      ::nexus::generated::CloseStdinInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleStreamKillOnUI(
      ::nexus::generated::StreamKillInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  // The persistent children (`spawn` with `persist`) — see
  // NexusPersistentShells.
  void HandlePersistentListOnUI(
      ::nexus::generated::ShellPersistentListInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandlePersistentKillOnUI(
      ::nexus::generated::ShellPersistentKillInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  // shell.env: read browser-process environment variables.
  void HandleEnvOnUI(
      ::nexus::generated::ShellEnvInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  // One streaming child: the controller remote (stdin / kill) and the
  // client receiver the service pushes output and the exit into. The
  // client pushes become `shell.onStreamOutput` / `shell.onStreamExit`
  // events on this adapter; the exit (or a dropped pipe) erases the entry.
  class StreamingChild;

  // Mojo reply to StreamSpawn: resolve `done` with { streamId }, or drop
  // the child and fire the error.
  void OnStreamSpawnDone(std::string stream_id,
                         ::nexus::BridgeResponseCallback done,
                         bool ok,
                         const std::optional<std::string>& error_code,
                         const std::optional<std::string>& error_message,
                         const std::optional<std::string>& install_hint);

  // Called by a StreamingChild: push one output event / the exit event.
  void PushStreamOutputEvent(const std::string& stream_id,
                             const char* stream,
                             const std::vector<uint8_t>& data);
  void PushStreamExitEvent(const std::string& stream_id,
                           const mojom::ShellExitInfo& exit);
  // The child exited or its pipe broke: forget it.
  void OnStreamingChildGone(const std::string& stream_id);

  // Broadcast sink registered with the shared launcher for the lifetime of this
  // adapter. The launcher calls it for EVERY background-shell terminal exit;
  // this pushes a `shell.onExit` bridge event to the guest, which filters by
  // shellId on the JS side. Registering at the bridge level (not per-shell)
  // avoids a spawn/poll race — see the launcher's `AddShellEventSink` doc.
  void PushShellExitEvent(const std::string& shell_id,
                          const mojom::ShellExitInfo& exit);

  // The Profile owns the shared launcher (Profile user-data); it outlives this
  // adapter (which is owned by the port service, itself bound to a render frame
  // of the profile). Used only to fetch `GetSharedShellLauncher(profile_)`.
  raw_ptr<Profile> profile_;

  // Streaming children by streamId. Destroying an entry drops the
  // controller remote and the client receiver; the service kills a child
  // whose controller went away, so a torn-down adapter leaves no orphan.
  std::map<std::string, std::unique_ptr<StreamingChild>> streams_;
  uint64_t next_stream_seq_ = 0;

  // Subscription id returned by the launcher's `AddShellEventSink`; used to
  // unregister in the destructor so a torn-down adapter stops receiving exits.
  // 0 means "no profile / not registered".
  uint64_t shell_event_sink_id_ = 0;

  // Guards the event-sink callback handed to the shared launcher — the launcher
  // is Profile-owned and outlives this adapter, so the callback must drop
  // cleanly if the adapter is destroyed before a shell exits.
  base::WeakPtrFactory<NexusShellBridgeAdapter> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_SHELL_NEXUS_SHELL_BRIDGE_ADAPTER_H_
