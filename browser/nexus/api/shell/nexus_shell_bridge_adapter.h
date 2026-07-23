// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_SHELL_NEXUS_SHELL_BRIDGE_ADAPTER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_SHELL_NEXUS_SHELL_BRIDGE_ADAPTER_H_

#include <cstdint>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "custom_browser/browser/nexus/api/shell/shell_api.gen.h"
#include "custom_browser/browser/nexus/bridge_response_callback.h"
#include "custom_browser/browser/nexus/mojom/nexus_shell.mojom.h"

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
// Scope is status + kill ONLY — NOT spawn (that stays the Bash tool's job).
// This is a narrow guest-side observe/control API like fs.* / model.*, not a
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

 private:
  void HandleStatusOnUI(
      ::nexus::generated::StatusInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleKillOnUI(
      ::nexus::generated::KillInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

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
