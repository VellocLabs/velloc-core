// Copyright 2024 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/nexus_bridge_port_service.h"

#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "custom_browser/browser/nexus/api/alarms/nexus_alarm_service.h"
#include "custom_browser/browser/nexus/api/alarms/nexus_alarms_bridge_adapter.h"
#include "custom_browser/browser/nexus/api/fs/nexus_fs_bridge_adapter.h"
#include "custom_browser/browser/nexus/api/sqlite/nexus_sqlite_bridge_adapter.h"
#include "custom_browser/browser/nexus/api/llm/nexus_llm_bridge_adapter.h"
#include "custom_browser/browser/nexus/api/app_info/nexus_app_info_bridge_adapter.h"
#include "custom_browser/browser/nexus/api/appearance/nexus_appearance_bridge_adapter.h"
#include "custom_browser/browser/nexus/api/browser_tools/nexus_browser_tools_bridge_adapter.h"
#include "custom_browser/browser/nexus/api/system_prefs/nexus_system_prefs_bridge_adapter.h"
#include "custom_browser/browser/nexus/api/network/nexus_network_bridge_adapter.h"
#include "custom_browser/browser/nexus/api/providers/nexus_providers_bridge_adapter.h"
#include "custom_browser/browser/nexus/api/remote_access/nexus_remote_access_bridge_adapter.h"
#include "custom_browser/browser/nexus/api/search/nexus_search_bridge_adapter.h"
#include "custom_browser/browser/nexus/api/secrets/nexus_secrets_bridge_adapter.h"
#include "custom_browser/browser/nexus/api/session/nexus_session_bridge_adapter.h"
#include "custom_browser/browser/nexus/api/shell/nexus_shell_bridge_adapter.h"
#include "custom_browser/browser/nexus/nexus_bridge_capability_handler.h"
#include "custom_browser/browser/nexus/api/browser/nexus_browser_bridge_adapter.h"
#include "custom_browser/browser/nexus/api/exec/nexus_exec_bridge_adapter.h"
#include "custom_browser/browser/nexus/api/files/nexus_files_bridge_adapter.h"
#include "custom_browser/browser/nexus/tools/tool_session_registry.h"
#include "custom_browser/log/bridge_audit_log.h"
#include "base/strings/string_number_conversions.h"
#include "base/time/time.h"
#include "crypto/sha2.h"
#include "custom_browser/browser/nexus/nexus_bridge_support.h"
#include "custom_browser/browser/nexus/nexus_message_port_endpoint.h"
#include "custom_browser/browser/nexus/nexus_secret_crypt.h"
#include "custom_browser/browser/nexus/api/ui/nexus_ui_bridge_adapter.h"

namespace nexus {

namespace {

using base::Value;
using Dict = base::DictValue;

constexpr char kBridgeNamespace[] = "nexus";

// The envelope tag every bridge frame carries.
bool HasBridgeTag(const Dict& request) {
  return request.FindBool("hostBridge").value_or(false);
}

void SetBridgeTags(Dict& envelope) {
  envelope.Set("hostBridge", true);
}

// The audit digest of a request's arguments: the handful of fields that
// identify what was touched (a path, a pattern, an element, a tab) and a
// hash of any command — never file contents, command text, secrets,
// headers or env.
std::string AuditDigest(const Dict& payload) {
  static constexpr std::string_view kPlainKeys[] = {
      "file_path", "path", "pattern", "glob", "url", "index", "tab_id",
      "from_index", "to_index", "cwd"};
  std::string digest;
  for (std::string_view key : kPlainKeys) {
    const Value* v = payload.Find(key);
    if (!v) {
      continue;
    }
    std::string text;
    if (v->is_string()) {
      text = v->GetString();
    } else if (v->is_int()) {
      text = base::NumberToString(v->GetInt());
    } else if (v->is_double()) {
      text = base::NumberToString(v->GetDouble());
    } else {
      continue;
    }
    if (!digest.empty()) {
      digest.push_back(',');
    }
    digest.append(key);
    digest.push_back(':');
    digest.append(text);
  }
  if (const std::string* command = payload.FindString("command")) {
    if (!digest.empty()) {
      digest.push_back(',');
    }
    digest.append("command_sha:");
    digest.append(
        base::HexEncode(crypto::SHA256HashString(*command)).substr(0, 12));
  }
  return digest;
}

Dict BuildResponseBase(const std::string& id, const Dict& route) {
  Dict response;
  SetBridgeTags(response);
  response.Set("direction", "response");
  response.Set("id", id);
  response.Set("route", route.Clone());
  return response;
}

Dict BuildErrorResponse(const std::string& id,
                        const Dict& route,
                        const std::string& code,
                        const std::string& message,
                        const Dict* params = nullptr) {
  Dict response = BuildResponseBase(id, route);
  response.Set("ok", false);

  Dict error;
  error.Set("code", code);
  error.Set("message", message);
  if (params && !params->empty()) {
    error.Set("data", params->Clone());
  }
  response.Set("error", std::move(error));
  return response;
}

Dict BuildSuccessResponse(const std::string& id,
                          const Dict& route,
                          Value result) {
  Dict response = BuildResponseBase(id, route);
  response.Set("ok", true);
  response.Set("result", std::move(result));
  return response;
}

}  // namespace

NexusBridgePortService::NexusBridgePortService(Profile* profile)
    : NexusBridgePortService(profile, PortServiceMode::kGuest) {}

NexusBridgePortService::NexusBridgePortService(Profile* profile,
                                               PortServiceMode mode) {
  // Prime the at-rest secret encryptor as early as possible (this is a
  // high-level entry point that can reach g_browser_process without a
  // dependency cycle). The LLM broker + MCP adapter rely on it being
  // acquired so their `WhenReady` re-reads + lazy decrypts resolve.
  NexusSecretCrypt::GetInstance()->EnsureAcquired(
      g_browser_process ? g_browser_process->os_crypt_async() : nullptr);

  base::FilePath cwd;
  if (profile && profile->GetPath().IsAbsolute()) {
    // Anchor the runtime cwd at the product root (the parent of the
    // chromium user-data-dir), not at the profile dir. `profile->GetPath()`
    // is `<user-data-dir>/<profile>`; `.DirName()` drops the profile name,
    // and a second `.DirName()` drops the user-data-dir name, landing on
    // the install root (e.g. `C:\Users\<u>\AppData\Local\Velloc\`). The
    // guest then resolves the `.nexus` storage path (skills, sessions,
    // memory, mcp config) under that root — shared across all chromium
    // profiles — instead of burying it under `User Data\Default`.
    cwd = profile->GetPath().DirName().DirName();
  }

  // A5: the panel port carries a strictly smaller capability surface than
  // the guest port. kPanel mode skips constructing the guest-only adapters
  // (fs/llm/network/session/catalog/browser/ui) so the panel port
  // service stays light — no NexusLlmBroker observer etc. — and only
  // registers the settings-page adapters that the WebUI panel actually
  // needs.
  if (mode == PortServiceMode::kPanel) {
    // P2: providers capability (replaces `getAgentProviders` chrome.send +
    // the rest of the provider RPCs as P3-P5 land). All actions run on UI
    // because they touch LlmBroker / PrefService which are UI-bound. See
    // api/providers/nexus_providers_bridge_adapter.h.
    providers_adapter_ =
        std::make_unique<NexusProvidersBridgeAdapter>(this, profile);
    RegisterCapability("providers", providers_adapter_.get());

    // Tier 2: browserTools capability (replaces the
    // `get/setBrowserToolsSeparateWindow` chrome.send routes used by
    // the Settings · Tools toggle + the topbar's one-shot read). Single
    // boolean field today; snapshot/setField shape mirrors the existing
    // `agent` and `features` capabilities. See
    // api/browserTools/nexus_browser_tools_bridge_adapter.h.
    browser_tools_adapter_ =
        std::make_unique<NexusBrowserToolsBridgeAdapter>(this, profile);
    RegisterCapability("browserTools", browser_tools_adapter_.get());

    return;
  }

  fs_adapter_ = std::make_unique<NexusFsBridgeAdapter>(this, cwd);
  llm_adapter_ = std::make_unique<NexusLlmBridgeAdapter>(this, profile);
  ui_adapter_ = std::make_unique<NexusUiBridgeAdapter>(this, profile);
  // V1 WebFetch: acquires its SharedURLLoaderFactory from `profile`
  // (default storage partition's browser-process loader), so the
  // egress path matches the LLM providers'. See
  // api/network/nexus_network_bridge_adapter.cc for details.
  network_adapter_ =
      std::make_unique<NexusNetworkBridgeAdapter>(this, profile);
  // The host tools: `files` / `exec` / `browser` are the API families behind
  // the runtime's tool definitions. `ToolSessionRegistry` owns the
  // per-session `ToolSession` instances (one `BrowserTool` per session) and
  // resolves each request's session from the frame context (else the
  // legacy `executionContext.sessionId`). Session lifecycle (`onStop`,
  // `setAgentLoopSettings`) lives on the dedicated `session`
  // capability below, not under `browser`.
  tool_sessions_ = std::make_unique<ToolSessionRegistry>(profile);
  files_adapter_ =
      std::make_unique<NexusFilesBridgeAdapter>(this, tool_sessions_.get());
  exec_adapter_ =
      std::make_unique<NexusExecBridgeAdapter>(this, tool_sessions_.get());
  browser_adapter_ =
      std::make_unique<NexusBrowserBridgeAdapter>(this, tool_sessions_.get());
  // Phase 12f: dedicated `session` capability. Hosts `onStop` (rename of
  // pre-12f `browser.endSession`), `setAgentLoopSettings` and
  // `setActiveSession`. Holds a back-pointer to
  // `tools_adapter_` for the lifecycle handlers.
  session_adapter_ = std::make_unique<NexusSessionBridgeAdapter>(
      this, profile, tool_sessions_.get());
  // Guest `shell` capability — the child-process primitive (streaming spawn,
  // persistent children, shell.env) over the per-Profile shared launcher.
  shell_adapter_ = std::make_unique<NexusShellBridgeAdapter>(this, profile);
  // Span-index transcript search. A pure forwarder: the engine, the span
  // cache and the `.sdi` sidecars all live in a utility process reached
  // through the per-Profile shared launcher.
  search_adapter_ = std::make_unique<NexusSearchBridgeAdapter>(this, profile);
  // Guest `sqlite` capability — read/write host SQLite files. Holds no
  // profile/cwd state: paths arrive absolute from the guest.
  sqlite_adapter_ = std::make_unique<NexusSqliteBridgeAdapter>(this);
  RegisterCapability("fs", fs_adapter_.get());
  RegisterCapability("model", llm_adapter_.get());
  RegisterCapability("network", network_adapter_.get());
  RegisterCapability("session", session_adapter_.get());
  RegisterCapability("search", search_adapter_.get());
  RegisterCapability("shell", shell_adapter_.get());
  RegisterCapability("sqlite", sqlite_adapter_.get());
  // Guest `secrets` capability — namespaced, origin-bound secrets sealed in
  // the profile prefs; no get (P-3). The network adapter
  // reads the same store to substitute `{{secret:NAME}}`.
  secrets_adapter_ = std::make_unique<NexusSecretsBridgeAdapter>(
      this, profile ? profile->GetPrefs() : nullptr);
  RegisterCapability("secrets", secrets_adapter_.get());
  // Guest `alarms` capability — persisted wall-clock alarms over the
  // per-Profile NexusAlarmService; this adapter is the service's sink while
  // the guest is connected (alarms.onAlarm).
  alarms_adapter_ = std::make_unique<NexusAlarmsBridgeAdapter>(
      this, NexusAlarmServiceFactory::GetForProfile(profile));
  RegisterCapability("alarms", alarms_adapter_.get());
  // Guest `remoteAccess` capability — status + configure for the embedded
  // relay. The runtime gates its getStatus call on the manifest advertisement
  // (the no-version-bump gate), so registering unconditionally is safe.
  remote_access_adapter_ =
      std::make_unique<NexusRemoteAccessBridgeAdapter>(this, profile);
  RegisterCapability("remoteAccess", remote_access_adapter_.get());
  RegisterCapability("ui", ui_adapter_.get());
  RegisterCapability("files", files_adapter_.get());
  RegisterCapability("exec", exec_adapter_.get());
  RegisterCapability("browser", browser_adapter_.get());

  // Settings · Tools "Open agent tabs in a separate window" toggle. The same
  // adapter the panel registers above, now also reachable on the guest direct
  // bridge so the React Tools section can persist `separateWindow` (panel +
  // guest paths are mutually exclusive, so reusing the member is safe).
  browser_tools_adapter_ =
      std::make_unique<NexusBrowserToolsBridgeAdapter>(this, profile);
  RegisterCapability("browserTools", browser_tools_adapter_.get());

  // Models page live verify + OAuth (read-only network/keychain probes). The
  // SAME provider adapter the panel registers, exposed on the guest direct
  // bridge under the non-colliding wire name "providerAuth" — the SharedWorker
  // keeps the "providers" capability (settings.json persistence), so there is
  // no routing collision. React calls
  // providerAuth.{verify,beginOAuth,oauthStatus,refresh,reset}; list/update/
  // setActive remain reachable but unused (the worker owns persistence).
  providers_adapter_ =
      std::make_unique<NexusProvidersBridgeAdapter>(this, profile);
  RegisterCapability("providerAuth", providers_adapter_.get());

  // Settings · System (browser-global local_state prefs: background-mode +
  // tray launcher) + Settings · About (version strings + the webapp-URL
  // editor). C++-served on the guest direct bridge — these are outside the
  // SharedWorker's settings.json domain. The React capability sets already
  // list `systemPrefs` + `appInfo`; this lights them up.
  system_prefs_adapter_ =
      std::make_unique<NexusSystemPrefsBridgeAdapter>(this, profile);
  RegisterCapability("systemPrefs", system_prefs_adapter_.get());
  app_info_adapter_ =
      std::make_unique<NexusAppInfoBridgeAdapter>(this, profile);
  RegisterCapability("appInfo", app_info_adapter_.get());

  // Settings · Appearance (colour scheme + user colour) and the language row
  // on Settings · General. Per-PROFILE, unlike systemPrefs above: ThemeService
  // is profile-keyed and kApplicationLocale is a profile pref. Write-only in
  // practice — the guest's repaint comes from the panel's existing nexus-theme
  // push, not from a read here.
  appearance_adapter_ =
      std::make_unique<NexusAppearanceBridgeAdapter>(this, profile);
  RegisterCapability("appearance", appearance_adapter_.get());
}

NexusBridgePortService::~NexusBridgePortService() {
  ResetPort();
}

blink::MessagePortDescriptor NexusBridgePortService::CreatePortPair() {
  ResetPort();

  blink::MessagePortDescriptorPair port_pair;
  browser_port_ = std::make_unique<NexusMessagePortEndpoint>(
      port_pair.TakePort0(),
      base::BindRepeating(&NexusBridgePortService::OnControlPortMessage,
                          weak_factory_.GetWeakPtr()),
      base::BindRepeating(&NexusBridgePortService::OnControlPortError,
                          weak_factory_.GetWeakPtr()));
  LOG(INFO) << "[NexusTrace][PortService] Created direct bridge port pair";
  NotifyConnectionOpened();
  return port_pair.TakePort1();
}

void NexusBridgePortService::ResetPort() {
  if (!browser_port_) {
    return;
  }

  browser_port_->Close();
  browser_port_.reset();
  LOG(INFO) << "[NexusTrace][PortService] Reset direct bridge port";
  NotifyConnectionClosed();
}

void NexusBridgePortService::NotifyConnectionClosed() {
  // OnBridgeConnectionClosed is idempotent, so an adapter registered under
  // two wire names being told twice is harmless.
  for (const auto& [capability, handler] : capability_handlers_) {
    if (handler) {
      handler->OnBridgeConnectionClosed();
    }
  }
}

void NexusBridgePortService::NotifyConnectionOpened() {
  for (const auto& [capability, handler] : capability_handlers_) {
    if (handler) {
      handler->OnBridgeConnectionOpened();
    }
  }
}

bool NexusBridgePortService::IsConnected() const {
  return browser_port_ && browser_port_->IsConnected();
}

void NexusBridgePortService::RegisterCapability(
    std::string_view capability,
    NexusBridgeCapabilityHandler* handler) {
  capability_handlers_.emplace(capability, handler);
}

void NexusBridgePortService::OnControlPortMessage(
    blink::WebMessagePayload payload,
    std::vector<blink::MessagePortDescriptor> ports) {
  std::optional<std::string> json_request =
      bridge_support::PayloadToUtf8(payload);
  if (!json_request) {
    LOG(ERROR) << "[NexusBridgePort] Control port received non-text payload";
    return;
  }

  DispatchRequest(*json_request, std::move(ports));
}

void NexusBridgePortService::OnControlPortError() {
  LOG(WARNING) << "[NexusBridgePort] Browser bridge port disconnected";
  // The guest went away: release what its connection held (fs watches,
  // sockets). A later CreatePortPair starts the next connection clean.
  NotifyConnectionClosed();
}

void NexusBridgePortService::DispatchRequest(
    const std::string& json_request,
    std::vector<blink::MessagePortDescriptor> ports) {
  auto parsed = base::JSONReader::ReadAndReturnValueWithError(
      json_request, base::JSON_PARSE_RFC);
  if (!parsed.has_value() || !parsed->is_dict()) {
    LOG(ERROR) << "[NexusBridgePort] Failed to parse request JSON";
    return;
  }

  const Dict& request = parsed->GetDict();
  const std::string* id = request.FindString("id");
  const Dict* route = request.FindDict("route");
  if (!id || id->empty() || !route) {
    LOG(ERROR) << "[NexusBridgePort] Missing id or route in bridge request";
    return;
  }

  const std::string* direction = request.FindString("direction");
  if (!HasBridgeTag(request) || !direction || *direction != "request") {
    SendErrorResponse(*id, *route, "INVALID_REQUEST",
                      "Invalid bridge request envelope");
    return;
  }

  const std::string* route_namespace = route->FindString("namespace");
  const std::string* capability = route->FindString("capability");
  const std::string* action = route->FindString("action");
  if (!route_namespace || *route_namespace != kBridgeNamespace || !capability ||
      !action) {
    SendErrorResponse(*id, *route, "INVALID_REQUEST", "Invalid bridge route");
    return;
  }

  const Value* payload_value = request.Find("payload");
  if (payload_value && !payload_value->is_dict()) {
    SendErrorResponse(*id, *route, "INVALID_REQUEST",
                      "Bridge payload must be an object");
    return;
  }

  Dict empty_payload;
  const Dict& payload =
      payload_value ? payload_value->GetDict() : empty_payload;
  // The frame-level call context (advisory; see
  // NexusBridgeCapabilityHandler::Dispatch).
  const Dict* context = request.FindDict("context");

  auto capability_it = capability_handlers_.find(*capability);
  if (capability_it == capability_handlers_.end()) {
    LOG(WARNING) << "[NexusTrace][PortService] Unknown capability: "
                 << *capability;
    SendErrorResponse(*id, *route, "NOT_IMPLEMENTED",
                      "No handler for capability: " + *capability);
    return;
  }

  // LOG(INFO) << "[NexusTrace][PortService] Dispatching route "
  //           << *route_namespace << "/" << *capability << "/" << *action
  //           << " request_id=" << *id;
  BeginAudit(*id, *route, payload, context,
             static_cast<int64_t>(json_request.size()));
  if (!capability_it->second->Dispatch(*action, *id, *route, payload,
                                       std::move(ports), context)) {
    SendErrorResponse(*id, *route, "NOT_IMPLEMENTED",
                      "Unsupported action for capability: " + *action);
  }
}

NexusBridgePortService::PendingAudit::PendingAudit() = default;
NexusBridgePortService::PendingAudit::PendingAudit(PendingAudit&&) noexcept =
    default;
NexusBridgePortService::PendingAudit&
NexusBridgePortService::PendingAudit::operator=(PendingAudit&&) noexcept =
    default;
NexusBridgePortService::PendingAudit::~PendingAudit() = default;

void NexusBridgePortService::BeginAudit(const std::string& id,
                                        const Dict& route,
                                        const Dict& payload,
                                        const Dict* context,
                                        int64_t request_bytes) {
  // A request that never gets a response (a crashed handler, a reset port)
  // would otherwise pin its entry forever; flush the oldest as abandoned.
  constexpr size_t kMaxPending = 2048;
  while (pending_audits_.size() >= kMaxPending) {
    FinishAudit(pending_audits_.begin()->first, "abandoned");
  }
  PendingAudit audit;
  audit.started = base::TimeTicks::Now();
  const std::string* capability = route.FindString("capability");
  const std::string* action = route.FindString("action");
  audit.capability = capability ? *capability : std::string();
  audit.action = action ? *action : std::string();
  audit.session_key =
      NexusBridgeCapabilityHandler::SessionKeyFor(&payload, context);
  audit.digest = AuditDigest(payload);
  audit.request_bytes = request_bytes;
  pending_audits_.insert_or_assign(id, std::move(audit));
}

void NexusBridgePortService::FinishAudit(const std::string& id,
                                         std::string_view outcome) {
  auto it = pending_audits_.find(id);
  if (it == pending_audits_.end()) {
    return;
  }
  const PendingAudit& audit = it->second;
  custom_browser::log::BridgeAuditLine line;
  line.Add("id", id)
      .Add("route", audit.capability + "." + audit.action)
      .Add("session", audit.session_key)
      .Add("args", audit.digest)
      .Add("request_bytes", audit.request_bytes)
      .Add("outcome", outcome)
      .Add("duration_ms",
           (base::TimeTicks::Now() - audit.started).InMilliseconds());
  std::move(line).Commit();
  pending_audits_.erase(it);
}

void NexusBridgePortService::SendSuccessResponse(const std::string& id,
                                                 const Dict& route,
                                                 Value result) {
  FinishAudit(id, "ok");
  SendResponse(bridge_support::SerializeJson(
      Value(BuildSuccessResponse(id, route, std::move(result)))));
}

void NexusBridgePortService::SendResponse(
    const std::string& json_response,
    std::vector<blink::MessagePortDescriptor> ports) {
  if (!browser_port_ || !browser_port_->IsConnected()) {
    LOG(WARNING) << "[NexusBridgePort] Dropping response because port is not "
                 << "connected";
    return;
  }

  if (!browser_port_->Send(bridge_support::Utf8Payload(json_response),
                           std::move(ports))) {
    LOG(ERROR) << "[NexusBridgePort] Failed to post bridge response";
  }
}

void NexusBridgePortService::SendBinaryResponse(
    const std::string& id,
    const Dict& route,
    Value result,
    mojo_base::BigBuffer data,
    std::string_view result_binary_field) {
  FinishAudit(id, "ok");
  blink::MessagePortDescriptorPair port_pair;
  std::vector<blink::MessagePortDescriptor> ports;
  ports.push_back(port_pair.TakePort1());

  if (result_binary_field == "$result") {
    SendResponse(bridge_support::SerializeJson(
                     Value(BuildSuccessResponse(id, route, std::move(result)))),
                 std::move(ports));
  } else {
    Dict result_dict;
    if (result.is_dict()) {
      result_dict = std::move(result).TakeDict();
    }
    SendResponse(bridge_support::SerializeJson(Value(BuildSuccessResponse(
                     id, route, Value(std::move(result_dict))))),
                 std::move(ports));
  }

  NexusMessagePortEndpoint data_port(
      port_pair.TakePort0(), NexusMessagePortEndpoint::MessageCallback(),
      NexusMessagePortEndpoint::ErrorCallback());
  if (!data_port.Send(bridge_support::BigBufferToPayload(std::move(data)))) {
    LOG(ERROR) << "[NexusBridgePort] Failed to send binary response payload";
  }
}

void NexusBridgePortService::SendErrorResponse(const std::string& id,
                                               const Dict& route,
                                               const std::string& code,
                                               const std::string& message) {
  FinishAudit(id, code);
  SendResponse(bridge_support::SerializeJson(
      Value(BuildErrorResponse(id, route, code, message))));
}

void NexusBridgePortService::SendErrorResponse(const std::string& id,
                                               const Dict& route,
                                               const std::string& code,
                                               const std::string& message,
                                               const Dict& params) {
  FinishAudit(id, code);
  SendResponse(bridge_support::SerializeJson(
      Value(BuildErrorResponse(id, route, code, message, &params))));
}

void NexusBridgePortService::SendEvent(const Dict& route, Value payload) {
  // Event envelope mirrors `DirectPortEventEnvelope` on the TS side
  // (`nexus_web/src/browser/bridge/DirectPortBridgeTransport.ts`):
  //   { hostBridge: true, direction: "event", route, payload }
  // No `id` field — events are not request-correlated. The TS dispatcher
  // routes by `route.capability` + `route.action`.
  Dict envelope;
  SetBridgeTags(envelope);
  envelope.Set("direction", "event");
  envelope.Set("route", route.Clone());
  envelope.Set("payload", std::move(payload));

  SendResponse(bridge_support::SerializeJson(Value(std::move(envelope))));
}

}  // namespace nexus
