#include "custom_browser/browser/nexus/llm/nexus_llm_bridge_adapter.h"

#include <utility>

#include "base/functional/bind.h"
#include "chrome/browser/profiles/profile.h"
#include "custom_browser/browser/nexus/llm/llm_broker.h"
#include "custom_browser/browser/nexus/llm/llm_broker_factory.h"
#include "custom_browser/browser/nexus/llm/nexus_llm_host.h"
#include "custom_browser/browser/nexus/ui/nexus_ui_broadcast.h"

namespace nexus {

namespace {

using base::Value;
using Dict = base::DictValue;

}  // namespace

NexusLlmBridgeAdapter::NexusLlmBridgeAdapter(
    NexusBridgeResponseDelegate* delegate,
    Profile* profile)
    : NexusBridgeCapabilityHandler(delegate),
      profile_(profile),
      llm_host_(std::make_unique<NexusLlmHost>(profile)) {
  Register("catalog", &NexusLlmBridgeAdapter::HandleCatalog);
  Register("call", &NexusLlmBridgeAdapter::HandleCall);
  Register("setDefaultSelection", "providerId", "modelId",
           &NexusLlmBridgeAdapter::HandleSetDefaultSelection);
  Register("streamStart", &NexusLlmBridgeAdapter::HandleStreamStart);
  Register("streamRead", "streamId", &NexusLlmBridgeAdapter::HandleStreamRead);
  // TS-pushed catalogue + session status. Both write into the broker's
  // in-memory + pref-backed caches so the WebUI quick-settings popup can
  // mirror the live picker without a guest round-trip.
  Register("publishCatalog", &NexusLlmBridgeAdapter::HandlePublishCatalog);
  Register("publishStatus", &NexusLlmBridgeAdapter::HandlePublishStatus);

  // Register ourselves as one of the broker's pick-event pushers.
  // Multiple bridge ports (page + worker) each construct their own
  // adapter; the broker fans every pick out to all of them so
  // whichever surface MultiSessionShell subscribes on receives the
  // rebind event. We hold on to our token so we can deregister cleanly
  // when this adapter is torn down — without this, an old WeakPtr-
  // bound callback would linger in the broker after a webview reload.
  if (LlmBroker* broker = LlmBrokerFactory::GetForProfile(profile_)) {
    pick_event_pusher_token_ = broker->AddPickEventPusher(
        base::BindRepeating(
            &NexusLlmBridgeAdapter::PushSessionApplySelection,
            weak_factory_.GetWeakPtr()));
  }
}

NexusLlmBridgeAdapter::~NexusLlmBridgeAdapter() {
  if (pick_event_pusher_token_.has_value() && profile_) {
    if (LlmBroker* broker = LlmBrokerFactory::GetForProfile(profile_)) {
      broker->RemovePickEventPusher(*pick_event_pusher_token_);
    }
  }
}

void NexusLlmBridgeAdapter::HandleCatalog(
    const std::string& id,
    const Dict& route,
    const Dict& /*payload*/,
    std::vector<blink::MessagePortDescriptor> /*ports*/) {
  llm_host_->Catalog(base::BindOnce(
      [](base::WeakPtr<NexusLlmBridgeAdapter> self, std::string id, Dict route,
         bool success, mojom::NexusLlmCatalogPtr catalog,
         std::optional<std::string> error_message) {
        if (!self) {
          return;
        }
        if (!success || !catalog) {
          self->SendErrorResponse(id, route, "LLM_ERROR",
                                  error_message.value_or("Catalog failed"));
          return;
        }

        Dict bridge_catalog = CatalogToBridge(*catalog);
        self->SendSuccessResponse(id, route, Value(std::move(bridge_catalog)));
      },
      weak_factory_.GetWeakPtr(), id, route.Clone()));
}

void NexusLlmBridgeAdapter::HandleCall(
    const std::string& id,
    const Dict& route,
    const Dict& payload,
    std::vector<blink::MessagePortDescriptor> /*ports*/) {
  std::string error_message;
  mojom::NexusLlmRequestParamsPtr request =
      BuildMojoLlmRequest(payload, &error_message);
  if (!request) {
    SendInvalidArgsResponse(id, route, error_message);
    return;
  }

  OnModelCallStarted();
  llm_host_->Call(
      std::move(request),
      base::BindOnce(
          [](base::WeakPtr<NexusLlmBridgeAdapter> self, std::string id,
             Dict route, bool success, mojom::NexusLlmCallResultPtr result,
             std::optional<std::string> error_message) {
            if (!self) {
              return;
            }
            self->OnModelCallFinished();
            if (!success || !result) {
              self->SendErrorResponse(id, route, "LLM_ERROR",
                                      error_message.value_or("Call failed"));
              return;
            }

            self->SendSuccessResponse(id, route,
                                      Value(CallResultToBridge(*result)));
          },
          weak_factory_.GetWeakPtr(), id, route.Clone()));
}

void NexusLlmBridgeAdapter::HandleStreamStart(
    const std::string& id,
    const Dict& route,
    const Dict& payload,
    std::vector<blink::MessagePortDescriptor> /*ports*/) {
  std::string error_message;
  mojom::NexusLlmRequestParamsPtr request =
      BuildMojoLlmRequest(payload, &error_message);
  if (!request) {
    SendInvalidArgsResponse(id, route, error_message);
    return;
  }

  llm_host_->StreamStart(
      std::move(request),
      base::BindOnce(
          [](base::WeakPtr<NexusLlmBridgeAdapter> self, std::string id,
             Dict route, bool success,
             std::optional<std::string> stream_id,
             std::optional<std::string> error_message) {
            if (!self) {
              return;
            }
            if (!success || !stream_id.has_value() || stream_id->empty()) {
              self->SendErrorResponse(
                  id, route, "LLM_ERROR",
                  error_message.value_or("StreamStart failed"));
              return;
            }

            self->bridge_stream_states_[*stream_id] =
                NexusLlmBridgeStreamState();
            self->BroadcastModelBusyIfChanged();

            Dict result;
            result.Set("streamId", *stream_id);
            self->SendSuccessResponse(id, route, Value(std::move(result)));
          },
          weak_factory_.GetWeakPtr(), id, route.Clone()));
}

void NexusLlmBridgeAdapter::HandleStreamRead(
    const std::string& id,
    const Dict& route,
    std::string stream_id,
    std::vector<blink::MessagePortDescriptor> /*ports*/) {
  if (bridge_stream_states_.find(stream_id) == bridge_stream_states_.end()) {
    bridge_stream_states_[stream_id] = NexusLlmBridgeStreamState();
  }

  llm_host_->StreamRead(
      stream_id,
      base::BindOnce(
          [](base::WeakPtr<NexusLlmBridgeAdapter> self, std::string id,
             Dict route, std::string stream_id, bool success,
             mojom::NexusLlmStreamReadResultPtr result,
             std::optional<std::string> error_message) {
            if (!self) {
              return;
            }
            if (!success || !result) {
              self->SendErrorResponse(id, route, "LLM_ERROR",
                                      error_message.value_or("StreamRead failed"));
              return;
            }

            auto state_it = self->bridge_stream_states_.find(stream_id);
            if (state_it == self->bridge_stream_states_.end()) {
              state_it = self->bridge_stream_states_
                             .emplace(stream_id, NexusLlmBridgeStreamState())
                             .first;
            }

            bool done = false;
            bool should_cancel = false;
            Dict bridge_result = StreamReadResultToBridge(
                &state_it->second, *result, &done, &should_cancel);

            if (done) {
              self->bridge_stream_states_.erase(state_it);
              self->BroadcastModelBusyIfChanged();
            }
            if (should_cancel) {
              self->llm_host_->Cancel(
                  stream_id,
                  base::BindOnce([](bool /*success*/,
                                    std::optional<std::string> /*error*/) {}));
            }

            self->SendSuccessResponse(id, route,
                                      Value(std::move(bridge_result)));
          },
          weak_factory_.GetWeakPtr(), id, route.Clone(), stream_id));
}

void NexusLlmBridgeAdapter::HandleSetDefaultSelection(
    const std::string& id,
    const Dict& route,
    std::string provider_id,
    std::string model_id,
    std::vector<blink::MessagePortDescriptor> /*ports*/) {
  std::string error_message;
  if (!llm_host_->SetDefaultSelection(provider_id, model_id, &error_message)) {
    SendErrorResponse(id, route, "LLM_ERROR",
                      error_message.empty() ?
                          "Failed to persist default model selection" :
                          error_message);
    return;
  }

  Dict result;
  result.Set("ok", true);
  SendSuccessResponse(id, route, Value(std::move(result)));
}

void NexusLlmBridgeAdapter::HandlePublishCatalog(
    const std::string& id,
    const Dict& route,
    const Dict& payload,
    std::vector<blink::MessagePortDescriptor> /*ports*/) {
  // Payload shape: { catalog: {...} } — the curated NEXUS_MODEL_CONFIG
  // serialized to JSON-friendly form on the TS side.
  const Dict* catalog = payload.FindDict("catalog");
  if (!catalog) {
    SendInvalidArgsResponse(id, route, "Missing 'catalog' object in payload");
    return;
  }

  LlmBroker* broker = LlmBrokerFactory::GetForProfile(profile_);
  if (!broker) {
    SendErrorResponse(id, route, "LLM_ERROR", "LlmBroker unavailable");
    return;
  }
  broker->SetTsModelCatalog(catalog->Clone());

  Dict result;
  result.Set("ok", true);
  SendSuccessResponse(id, route, Value(std::move(result)));
}

void NexusLlmBridgeAdapter::HandlePublishStatus(
    const std::string& id,
    const Dict& route,
    const Dict& payload,
    std::vector<blink::MessagePortDescriptor> /*ports*/) {
  // Payload shape: SessionStatus directly (sessionId + selection + future
  // extensible fields). We store it verbatim so future extensions don't
  // require adapter changes.
  LlmBroker* broker = LlmBrokerFactory::GetForProfile(profile_);
  if (!broker) {
    SendErrorResponse(id, route, "LLM_ERROR", "LlmBroker unavailable");
    return;
  }
  broker->SetSessionStatus(payload.Clone());

  Dict result;
  result.Set("ok", true);
  SendSuccessResponse(id, route, Value(std::move(result)));
}

void NexusLlmBridgeAdapter::OnModelCallStarted() {
  ++model_call_in_flight_;
  BroadcastModelBusyIfChanged();
}

void NexusLlmBridgeAdapter::OnModelCallFinished() {
  if (model_call_in_flight_ > 0) {
    --model_call_in_flight_;
  }
  BroadcastModelBusyIfChanged();
}

void NexusLlmBridgeAdapter::BroadcastModelBusyIfChanged() {
  const bool busy =
      model_call_in_flight_ > 0 || !bridge_stream_states_.empty();
  if (busy == last_broadcast_model_busy_) {
    return;
  }
  last_broadcast_model_busy_ = busy;

  Dict payload;
  payload.Set("modelBusy", busy);
  BroadcastWebUiListenerEventToProfile(
      profile_, "nexusAgentStateChanged", Value(std::move(payload)));
}

void NexusLlmBridgeAdapter::PushSessionApplySelection(
    const std::string& request_id,
    const std::string& provider_id,
    const std::string& model_id,
    const std::string& thinking_tier,
    bool also_update_default) {
  // Build the route + payload that mirror the TS-side
  // `session.applySelection` event envelope. The transport wraps this in
  // `{claudeBridge:true, direction:"event", route, payload}` automatically.
  Dict route;
  route.Set("namespace", "nexus");
  route.Set("capability", "session");
  route.Set("action", "applySelection");

  Dict payload;
  payload.Set("requestId", request_id);
  payload.Set("providerId", provider_id);
  payload.Set("modelId", model_id);
  if (!thinking_tier.empty()) {
    payload.Set("thinkingTier", thinking_tier);
  }
  payload.Set("alsoUpdateDefault", also_update_default);

  SendEvent(route, Value(std::move(payload)));
}

}  // namespace nexus
