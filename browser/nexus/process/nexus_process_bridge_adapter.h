// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_PROCESS_NEXUS_PROCESS_BRIDGE_ADAPTER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_PROCESS_NEXUS_PROCESS_BRIDGE_ADAPTER_H_

#include <memory>
#include <string>
#include <vector>

#include "base/memory/weak_ptr.h"
#include "custom_browser/browser/nexus/mojom/nexus_shell.mojom.h"
#include "custom_browser/browser/nexus/nexus_bridge_capability_handler.h"
#include "mojo/public/cpp/bindings/remote.h"

namespace blink {
class MessagePortDescriptor;
}

namespace nexus {

class NexusBridgeResponseDelegate;
class NexusShellServiceLauncher;

// Bridge handler for the `process` capability. Translates JSON request
// envelopes coming from the guest SharedWorker into Mojo calls on the
// `NexusShellService` utility-process remote.
//
// Supported actions (slice 2):
//   * `getCapabilities` — JSON pass-through of the resolved tools,
//     allowlist, and default-shell info; populates the system-prompt
//     environment context for the LLM.
//   * `exec` — one-shot, with stdout / stderr returned via
//     `SendBinaryResponse` (one transferred MessagePort carrying an
//     ArrayBuffer per stream, layered using the existing protocol).
//
// `spawn` (streaming) lands in slice 4.
//
// The adapter is constructed with a non-owning pointer to a
// `NexusShellServiceLauncher`; the launcher's lifetime must outlive
// the adapter (today that means: launcher is stored on the same
// `NexusBridgePortService` that owns this adapter).
class NexusProcessBridgeAdapter : public NexusBridgeCapabilityHandler {
 public:
  // The `service_factory` callback returns a long-lived
  // `mojo::Remote<NexusShellService>&` — typically backed by
  // `NexusShellServiceLauncher::GetService`. Using a callback rather
  // than a raw launcher pointer lets unit tests inject a fake remote
  // bound directly to a `NexusShellServiceImpl`.
  using ServiceFactory =
      base::RepeatingCallback<mojo::Remote<mojom::NexusShellService>&()>;

  NexusProcessBridgeAdapter(NexusBridgeResponseDelegate* delegate,
                            ServiceFactory service_factory);

  NexusProcessBridgeAdapter(const NexusProcessBridgeAdapter&) = delete;
  NexusProcessBridgeAdapter& operator=(const NexusProcessBridgeAdapter&) =
      delete;

  ~NexusProcessBridgeAdapter() override;

 private:
  void HandleGetCapabilities(
      const std::string& id,
      const base::DictValue& route,
      const base::DictValue& payload,
      std::vector<blink::MessagePortDescriptor> ports);
  void HandleExec(const std::string& id,
                  const base::DictValue& route,
                  const base::DictValue& payload,
                  std::vector<blink::MessagePortDescriptor> ports);

  // Helper that builds a `nexus.mojom.ShellSpawnOptions` from the JSON
  // payload. Returns a null pointer on validation failure and writes a
  // human-readable reason to `*error`. The adapter forwards that
  // reason to the guest as INVALID_ARGS rather than letting the utility
  // process re-validate.
  mojom::ShellSpawnOptionsPtr BuildSpawnOptions(const base::DictValue& payload,
                                                std::string* error) const;

  ServiceFactory service_factory_;
  base::WeakPtrFactory<NexusProcessBridgeAdapter> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_PROCESS_NEXUS_PROCESS_BRIDGE_ADAPTER_H_
