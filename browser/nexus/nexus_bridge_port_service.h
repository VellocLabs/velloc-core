#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_BRIDGE_PORT_SERVICE_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_BRIDGE_PORT_SERVICE_H_

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "custom_browser/browser/nexus/nexus_bridge_response_delegate.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "third_party/blink/public/common/messaging/message_port_descriptor.h"
#include "third_party/blink/public/common/messaging/string_message_codec.h"

class Profile;

namespace nexus {

class NexusBridgeCapabilityHandler;
class NexusCatalogBridgeAdapter;
class NexusFsBridgeAdapter;
class NexusLlmBridgeAdapter;
class NexusMessagePortEndpoint;
class NexusNetworkBridgeAdapter;
class NexusSessionBridgeAdapter;
class NexusUiBridgeAdapter;
class ToolsAdapter;

// A browser-side service that creates a MessagePort pair, keeps one end for
// handling BrowserBridgeRequest/BrowserBridgeResponse JSON directly from the
// guest SharedWorker, and dispatches to capability handlers (fs, model, etc.).
//
// Lifetime: Owned by the `NexusBridgeBootstrapHost` bound to the active
// `chrome://glic` render frame. Each `GuestReady()` bootstrap creates fresh
// page/worker bridge services and fresh entangled port pairs.
//
// The browser end receives JSON messages of the form:
//   { claudeBridge: true, direction: "request",
//     id, route: { namespace, capability, action }, payload }
// and responds with:
//   { claudeBridge: true, direction: "response",
//     id, route, ok, result/error }
class NexusBridgePortService : public NexusBridgeResponseDelegate {
 public:
  explicit NexusBridgePortService(Profile* profile);
  ~NexusBridgePortService() override;

  NexusBridgePortService(const NexusBridgePortService&) = delete;
  NexusBridgePortService& operator=(const NexusBridgePortService&) = delete;

  // Creates a new entangled port pair. Returns the "guest end" descriptor
  // that must be transferred to the guest webview/SharedWorker.
  // The "browser end" is retained and used for request/response handling.
  // If a previous port pair exists, it is invalidated first.
  blink::MessagePortDescriptor CreatePortPair();

  // Invalidates the current browser-side port (e.g., on webview reload).
  void ResetPort();

  // Returns true if the browser-side port is currently connected and
  // receiving messages.
  bool IsConnected() const;

  void SendSuccessResponse(const std::string& id,
                           const base::DictValue& route,
                           base::Value result) override;
  void SendBinaryResponse(const std::string& id,
                          const base::DictValue& route,
                          base::Value result,
                          mojo_base::BigBuffer data,
                          std::string_view result_binary_field) override;
  void SendErrorResponse(const std::string& id,
                         const base::DictValue& route,
                         const std::string& code,
                         const std::string& message) override;
  void SendEvent(const base::DictValue& route, base::Value payload) override;

 private:
  void RegisterCapability(std::string_view capability,
                          NexusBridgeCapabilityHandler* handler);
  void OnControlPortMessage(blink::WebMessagePayload payload,
                            std::vector<blink::MessagePortDescriptor> ports);
  void OnControlPortError();
  void DispatchRequest(const std::string& json_request,
                       std::vector<blink::MessagePortDescriptor> ports);
  void SendResponse(const std::string& json_response,
                    std::vector<blink::MessagePortDescriptor> ports = {});

  std::unique_ptr<NexusFsBridgeAdapter> fs_adapter_;
  std::unique_ptr<NexusLlmBridgeAdapter> llm_adapter_;
  std::unique_ptr<NexusUiBridgeAdapter> ui_adapter_;
  // V1 of WebFetch: general-purpose HTTP-GET bridge surface used by
  // TS-side tools that need to fetch a URL through the Chromium net
  // stack (not subject to renderer CORS). See
  // api/network/nexus_network_bridge_adapter.h for the contract and
  // api/declarations/network/httpGet.api.json for the wire schema.
  std::unique_ptr<NexusNetworkBridgeAdapter> network_adapter_;
  // Phase 12: serves the `catalog` capability — listHostedTools (lazy-
  // pull replacement for the bootstrap manifest's hostedTools field)
  // and renderPromptSection / listPromptSections (C++ side now owns
  // the prose-rendering that pre-Phase-12 lived in
  // nexus_web/src/constants/systemPromptToolSection.ts).
  std::unique_ptr<NexusCatalogBridgeAdapter> catalog_adapter_;
  // Owns the per-LLM-session sessions map (`ToolSession` per sessionId)
  // and routes catalog dispatchers from `ToolCatalog::Get()` through
  // session lookup before invoking the per-category methods (currently
  // `BrowserTool`'s 16 `browser_*` methods). The legacy
  // `NexusGlicBridgeAdapter` and the `glic` capability it served have
  // been retired — every browser-actor tool call now arrives via
  // `browser.<tool_name>` and lifecycle cleanup arrives via
  // `session.onStop` (Phase 12f, formerly `browser.endSession`).
  std::unique_ptr<ToolsAdapter> tools_adapter_;
  // Phase 12f: serves the `session` capability — `publishStatus`
  // (broker cache; pre-12f lived under `llm/`), `onStop` (rename of
  // pre-12f `browser.endSession`; cascades through ToolsAdapter), and
  // `setAgentLoopSettings` (per-session wall-clock budgets). Holds a
  // raw back-pointer to `tools_adapter_`; the two adapters share a
  // lifetime under this port service so the back-ref is safe.
  std::unique_ptr<NexusSessionBridgeAdapter> session_adapter_;
  // Phase 12g: dropped `process_adapter_` + `shell_service_launcher_`.
  // The bridge `process` capability had no JS-side callers (Phase 11b
  // deleted the only consumers — TS BashTool / GrepTool / GlobTool).
  // The C++ ProcessTool (under `tools/process_tool/`) talks to
  // NexusShellService directly through its own launcher; that path
  // is unchanged.
  std::map<std::string, NexusBridgeCapabilityHandler*, std::less<>>
      capability_handlers_;
  std::unique_ptr<NexusMessagePortEndpoint> browser_port_;
  base::WeakPtrFactory<NexusBridgePortService> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_BRIDGE_PORT_SERVICE_H_
