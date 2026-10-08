#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_BRIDGE_PORT_SERVICE_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_BRIDGE_PORT_SERVICE_H_

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/values.h"
#include "custom_browser/browser/nexus/nexus_bridge_response_delegate.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "third_party/blink/public/common/messaging/message_port_descriptor.h"
#include "third_party/blink/public/common/messaging/string_message_codec.h"

class Profile;

namespace nexus {

class NexusBridgeCapabilityHandler;
class NexusAppInfoBridgeAdapter;
class NexusAppearanceBridgeAdapter;
class NexusBrowserToolsBridgeAdapter;
class NexusFsBridgeAdapter;
class NexusLlmBridgeAdapter;
class NexusMessagePortEndpoint;
class NexusNetworkBridgeAdapter;
class NexusProvidersBridgeAdapter;
class NexusRemoteAccessBridgeAdapter;
class NexusSearchBridgeAdapter;
class NexusSecretsBridgeAdapter;
class NexusAlarmsBridgeAdapter;
class NexusSessionBridgeAdapter;
class NexusShellBridgeAdapter;
class NexusSqliteBridgeAdapter;
class NexusSystemPrefsBridgeAdapter;
class NexusUiBridgeAdapter;
class NexusBrowserBridgeAdapter;
class NexusExecBridgeAdapter;
class NexusFilesBridgeAdapter;
class ToolSessionRegistry;

// Approach 5 (A5): which "audience" this port service exposes capabilities
// to. Determines the capability registration set.
//
//   kGuest — full set (fs, model, network, mcp, session, ui, browser,
//            catalog). For the nexus_web webview's page thread and
//            SharedWorker. Same registration list as before A5.
//
//   kPanel — minimal set scoped to settings UI calls (initially: providers
//            only; will grow as more settings pages migrate off chrome.send).
//            For the WebUI panel (chrome://custom.top-chrome) itself, which
//            should NOT have access to filesystem, LLM streaming, browser
//            tools, etc. The settings panel is privileged but its threat
//            model is different — keep its capability surface tight.
//
// The dispatcher is otherwise identical. A request for an unregistered
// capability on a port returns NOT_IMPLEMENTED — same error envelope as a
// genuinely-missing capability — so a misrouted call surfaces a clear
// error instead of silently misbehaving.
enum class PortServiceMode {
  kGuest,
  kPanel,
};

// A browser-side service that creates a MessagePort pair, keeps one end for
// handling BrowserBridgeRequest/BrowserBridgeResponse JSON directly from the
// guest SharedWorker, and dispatches to capability handlers (fs, model, etc.).
//
// Lifetime: Owned by the `NexusBridgeBootstrapHost` bound to the active
// `chrome://custom.top-chrome` render frame. Each bootstrap creates fresh
// page/worker/panel bridge services and fresh entangled port pairs.
//
// The browser end receives JSON messages of the form:
//   { hostBridge: true, direction: "request",
//     id, route: { namespace, capability, action }, payload }
// and responds with:
//   { hostBridge: true, direction: "response",
//     id, route, ok, result/error }
class NexusBridgePortService : public NexusBridgeResponseDelegate {
 public:
  // Default constructor — `kGuest` mode (full capability set). Existing
  // callers (page + worker bridges) keep working without source changes.
  explicit NexusBridgePortService(Profile* profile);
  // A5: explicit-mode constructor. Use `kPanel` for the WebUI panel's port.
  NexusBridgePortService(Profile* profile, PortServiceMode mode);
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
  void SendErrorResponse(const std::string& id,
                         const base::DictValue& route,
                         const std::string& code,
                         const std::string& message,
                         const base::DictValue& params) override;
  void SendEvent(const base::DictValue& route, base::Value payload) override;

 private:
  void RegisterCapability(std::string_view capability,
                          NexusBridgeCapabilityHandler* handler);
  void OnControlPortMessage(blink::WebMessagePayload payload,
                            std::vector<blink::MessagePortDescriptor> ports);
  void OnControlPortError();
  // Tells every registered adapter that the guest connection ended
  // (NexusBridgeCapabilityHandler::OnBridgeConnectionClosed).
  void NotifyConnectionClosed();
  // ... and that a new one began (OnBridgeConnectionOpened).
  void NotifyConnectionOpened();
  void DispatchRequest(const std::string& json_request,
                       std::vector<blink::MessagePortDescriptor> ports);
  void SendResponse(const std::string& json_response,
                    std::vector<blink::MessagePortDescriptor> ports = {});

  // The bridge audit log (custom_browser/log/bridge_audit_log.h): one line
  // per request, written when its response goes out.
  struct PendingAudit {
    PendingAudit();
    PendingAudit(PendingAudit&&) noexcept;
    PendingAudit& operator=(PendingAudit&&) noexcept;
    ~PendingAudit();

    base::TimeTicks started;
    std::string capability;
    std::string action;
    std::string session_key;
    std::string digest;
    int64_t request_bytes = 0;
  };
  void BeginAudit(const std::string& id,
                  const base::DictValue& route,
                  const base::DictValue& payload,
                  const base::DictValue* context,
                  int64_t request_bytes);
  void FinishAudit(const std::string& id, std::string_view outcome);
  std::map<std::string, PendingAudit> pending_audits_;

  std::unique_ptr<NexusFsBridgeAdapter> fs_adapter_;
  std::unique_ptr<NexusLlmBridgeAdapter> llm_adapter_;
  std::unique_ptr<NexusUiBridgeAdapter> ui_adapter_;
  // A5: providers capability adapter. Constructed (and the capability
  // registered) only on `PortServiceMode::kPanel` — exposed to the WebUI
  // panel's direct port, not to the guest-facing page/worker ports.
  std::unique_ptr<NexusProvidersBridgeAdapter> providers_adapter_;
  // A5 / Tier 2: browserTools capability adapter (snapshot + setField,
  // single `separateWindow` field today). Registered on the panel port
  // ("browserTools") AND the guest direct bridge ("browserTools").
  std::unique_ptr<NexusBrowserToolsBridgeAdapter> browser_tools_adapter_;
  // Guest-only: Settings · System (browser-global local_state prefs) +
  // Settings · About (version + webapp-URL editor).
  std::unique_ptr<NexusSystemPrefsBridgeAdapter> system_prefs_adapter_;
  std::unique_ptr<NexusAppInfoBridgeAdapter> app_info_adapter_;
  std::unique_ptr<NexusAppearanceBridgeAdapter> appearance_adapter_;
  // V1 of WebFetch: general-purpose HTTP-GET bridge surface used by
  // TS-side tools that need to fetch a URL through the Chromium net
  // stack (not subject to renderer CORS). See
  // api/network/nexus_network_bridge_adapter.h for the contract and
  // api/declarations/network/httpGet.api.json for the wire schema.
  std::unique_ptr<NexusNetworkBridgeAdapter> network_adapter_;
  // Owns the per-LLM-session sessions map (`ToolSession` per sessionId);
  // the `files` / `exec` / `browser` adapters resolve a call's session here
  // before invoking the tool class. The legacy `NexusGlicBridgeAdapter` and
  // the `glic` capability it served have been retired — every host tool
  // call arrives on its family's action and lifecycle cleanup arrives via
  // `session.onStop` (Phase 12f, formerly `browser.endSession`).
  // The per-connection host tool sessions (one ToolSession per runtime
  // session id), shared by the three tool adapters below and the `session`
  // adapter. Declared before them so it outlives them.
  std::unique_ptr<ToolSessionRegistry> tool_sessions_;
  // W16: the host actions behind the hosted tools, one capability per
  // family. `browser` also serves the legacy `browser.<ToolName>` routes
  // until W16 step 3.
  std::unique_ptr<NexusFilesBridgeAdapter> files_adapter_;
  std::unique_ptr<NexusExecBridgeAdapter> exec_adapter_;
  std::unique_ptr<NexusBrowserBridgeAdapter> browser_adapter_;
  // Phase 12f: serves the `session` capability — `onStop` (rename of
  // pre-12f `browser.endSession`; cascades through ToolSessionRegistry),
  // `setAgentLoopSettings` (per-session wall-clock budgets) and
  // `setActiveSession`. Holds a
  // raw back-pointer to `tools_adapter_`; the two adapters share a
  // lifetime under this port service so the back-ref is safe.
  std::unique_ptr<NexusSessionBridgeAdapter> session_adapter_;
  // Guest-only: the `search` capability — span-index search over the
  // on-disk session transcripts (the Ctrl+K dialog). All work runs on a
  // dedicated ThreadPool sequence inside the adapter.
  std::unique_ptr<NexusSearchBridgeAdapter> search_adapter_;
  // Guest-only: the `shell` capability — the child-process primitive
  // (streaming `spawn`, stdin / kill, the persistent children, `env`) over the
  // per-Profile shared NexusShellServiceLauncher. NOT a revival of the
  // Phase-12g `process` capability (which was redundant tool-dispatch).
  std::unique_ptr<NexusShellBridgeAdapter> shell_adapter_;
  std::unique_ptr<NexusSqliteBridgeAdapter> sqlite_adapter_;
  // Guest `secrets` capability (set / has / delete / listNames).
  std::unique_ptr<NexusSecretsBridgeAdapter> secrets_adapter_;
  // Guest `alarms` capability (create / clear / list + alarms.onAlarm).
  std::unique_ptr<NexusAlarmsBridgeAdapter> alarms_adapter_;
  // Guest `remoteAccess` capability — status + configure for the desktop's
  // embedded relay (remote-browser-client plan §8). Forwards onto the
  // per-Profile NexusRemoteAccessService. Manifest-gated: the runtime calls
  // getStatus only when the manifest advertises "remoteAccess".
  std::unique_ptr<NexusRemoteAccessBridgeAdapter> remote_access_adapter_;
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
