// Copyright 2024 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/nexus_bridge_port_service.h"

#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "chrome/browser/profiles/profile.h"
#include "custom_browser/browser/nexus/fs/nexus_fs_bridge_adapter.h"
#include "custom_browser/browser/nexus/llm/nexus_llm_bridge_adapter.h"
#include "custom_browser/browser/nexus/nexus_bridge_capability_handler.h"
#include "custom_browser/browser/nexus/nexus_bridge_support.h"
#include "custom_browser/browser/nexus/nexus_message_port_endpoint.h"
#include "custom_browser/browser/nexus/mojom/nexus_shell.mojom.h"
#include "custom_browser/browser/nexus/nexus_features.h"
#include "custom_browser/browser/nexus/process/nexus_process_bridge_adapter.h"
#include "custom_browser/browser/nexus/process/nexus_shell_service_launcher.h"
#include "custom_browser/browser/nexus/ui/nexus_ui_bridge_adapter.h"

namespace nexus {

namespace {

using base::Value;
using Dict = base::DictValue;

constexpr char kBridgeNamespace[] = "nexus";

Dict BuildResponseBase(const std::string& id, const Dict& route) {
  Dict response;
  response.Set("claudeBridge", true);
  response.Set("direction", "response");
  response.Set("id", id);
  response.Set("route", route.Clone());
  return response;
}

Dict BuildErrorResponse(const std::string& id,
                        const Dict& route,
                        const std::string& code,
                        const std::string& message) {
  Dict response = BuildResponseBase(id, route);
  response.Set("ok", false);

  Dict error;
  error.Set("code", code);
  error.Set("message", message);
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

NexusBridgePortService::NexusBridgePortService(Profile* profile) {
  base::FilePath cwd;
  if (profile && profile->GetPath().IsAbsolute()) {
    cwd = profile->GetPath();
  }

  fs_adapter_ = std::make_unique<NexusFsBridgeAdapter>(this, cwd);
  llm_adapter_ = std::make_unique<NexusLlmBridgeAdapter>(this, profile);
  ui_adapter_ = std::make_unique<NexusUiBridgeAdapter>(this, profile);
  RegisterCapability("fs", fs_adapter_.get());
  RegisterCapability("model", llm_adapter_.get());
  RegisterCapability("ui", ui_adapter_.get());

  // The `process` capability is gated behind a Chromium feature flag.
  // When the flag is off we skip both the launcher *and* the adapter so
  // the utility process is never spawned and the guest's
  // `requireCapability('process')` rejects with PROCESS_UNAVAILABLE
  // instead of silently appearing to work. The flag has no kill-switch
  // dependency on profile state — we re-check at construction so that
  // toggling via about:flags on a relaunch is sufficient.
  if (features::IsNexusProcessCapabilityEnabled()) {
    shell_service_launcher_ = std::make_unique<NexusShellServiceLauncher>();
    process_adapter_ = std::make_unique<NexusProcessBridgeAdapter>(
        this, base::BindRepeating(
                  [](NexusShellServiceLauncher* launcher)
                      -> mojo::Remote<mojom::NexusShellService>& {
                    return launcher->GetService();
                  },
                  shell_service_launcher_.get()));
    RegisterCapability("process", process_adapter_.get());
  } else {
    LOG(INFO) << "[NexusBridgePortService] process capability disabled "
                 "(PrereleaseNexusProcessCapability flag off); guest "
                 "process tools will receive PROCESS_UNAVAILABLE.";
  }
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
  return port_pair.TakePort1();
}

void NexusBridgePortService::ResetPort() {
  if (!browser_port_) {
    return;
  }

  browser_port_->Close();
  browser_port_.reset();
  LOG(INFO) << "[NexusTrace][PortService] Reset direct bridge port";
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
  if (!request.FindBool("claudeBridge").value_or(false) || !direction ||
      *direction != "request") {
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

  auto capability_it = capability_handlers_.find(*capability);
  if (capability_it == capability_handlers_.end()) {
    LOG(WARNING) << "[NexusTrace][PortService] Unknown capability: "
                 << *capability;
    SendErrorResponse(*id, *route, "NOT_IMPLEMENTED",
                      "No handler for capability: " + *capability);
    return;
  }

  LOG(INFO) << "[NexusTrace][PortService] Dispatching route "
            << *route_namespace << "/" << *capability << "/" << *action
            << " request_id=" << *id;
  if (!capability_it->second->Dispatch(*action, *id, *route, payload,
                                       std::move(ports))) {
    SendErrorResponse(*id, *route, "NOT_IMPLEMENTED",
                      "Unsupported action for capability: " + *action);
  }
}

void NexusBridgePortService::SendSuccessResponse(const std::string& id,
                                                 const Dict& route,
                                                 Value result) {
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
  SendResponse(bridge_support::SerializeJson(
      Value(BuildErrorResponse(id, route, code, message))));
}

}  // namespace nexus
