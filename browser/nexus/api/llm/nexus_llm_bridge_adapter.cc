#include "custom_browser/browser/nexus/api/llm/nexus_llm_bridge_adapter.h"

#include <utility>

#include "base/functional/bind.h"
#include "chrome/browser/profiles/profile.h"
#include "custom_browser/browser/nexus/api/catalog/prompt_holder_registry.h"
#include "custom_browser/browser/nexus/binaries/nexus_binary_config_registry.h"
#include "custom_browser/browser/nexus/api/llm/llm_broker.h"
#include "custom_browser/browser/nexus/api/llm/llm_broker_factory.h"
#include "custom_browser/browser/nexus/api/llm/nexus_llm_host.h"

namespace nexus {

namespace {

using base::Value;
using Dict = base::DictValue;

}  // namespace

NexusLlmBridgeAdapter::NexusLlmBridgeAdapter(
    NexusBridgeResponseDelegate* delegate,
    Profile* profile)
    : ::nexus::generated::LlmApiBase(delegate),
      profile_(profile),
      llm_host_(std::make_unique<NexusLlmHost>(profile)) {
  // Register(...) calls live in `LlmApiBase`'s ctor, generated from
  // api/declarations/llm/*.api.json. Adding a new llm wire action: drop
  // a *.api.json under api/declarations/llm/, add a Handle*OnUI()
  // override here. The codegen wires the dispatch automatically.

  // Register ourselves as one of the broker's pick-event pushers.
  // Multiple bridge ports (page + worker) each construct their own
  // adapter; the broker fans every pick out to all of them so
  // whichever surface MultiSessionShell subscribes on receives the
  // rebind event.
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

void NexusLlmBridgeAdapter::HandleCatalogOnUI(
    ::nexus::generated::CatalogInput /*input*/,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  llm_host_->Catalog(base::BindOnce(
      [](::nexus::BridgeResponseCallback done, bool success,
         mojom::NexusLlmCatalogPtr catalog,
         std::optional<std::string> error_message) {
        if (!success || !catalog) {
          std::move(done).Error("LLM_ERROR",
                                error_message.value_or("Catalog failed"));
          return;
        }
        std::move(done).Success(Value(CatalogToBridge(*catalog)));
      },
      std::move(done)));
}

void NexusLlmBridgeAdapter::HandleCallOnUI(
    ::nexus::generated::CallInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  std::string error_message;
  mojom::NexusLlmRequestParamsPtr request =
      BuildMojoLlmRequest(input.payload, &error_message);
  if (!request) {
    std::move(done).InvalidArgs(error_message);
    return;
  }

  llm_host_->Call(
      std::move(request),
      base::BindOnce(
          [](::nexus::BridgeResponseCallback done, bool success,
             mojom::NexusLlmCallResultPtr result,
             std::optional<std::string> error_message) {
            if (!success || !result) {
              std::move(done).Error("LLM_ERROR",
                                    error_message.value_or("Call failed"));
              return;
            }
            std::move(done).Success(Value(CallResultToBridge(*result)));
          },
          std::move(done)));
}

void NexusLlmBridgeAdapter::HandleStreamStartOnUI(
    ::nexus::generated::StreamStartInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  std::string error_message;
  mojom::NexusLlmRequestParamsPtr request =
      BuildMojoLlmRequest(input.payload, &error_message);
  if (!request) {
    std::move(done).InvalidArgs(error_message);
    return;
  }

  // Capture self via WeakPtr so the inner callback can read/write
  // bridge_stream_states_ when the host's StreamStart resolves. `done`
  // is captured separately and consumed at the end.
  llm_host_->StreamStart(
      std::move(request),
      base::BindOnce(
          [](base::WeakPtr<NexusLlmBridgeAdapter> self,
             ::nexus::BridgeResponseCallback done, bool success,
             std::optional<std::string> stream_id,
             std::optional<std::string> error_message) {
            if (!success || !stream_id.has_value() || stream_id->empty()) {
              std::move(done).Error(
                  "LLM_ERROR",
                  error_message.value_or("StreamStart failed"));
              return;
            }
            if (self) {
              self->bridge_stream_states_[*stream_id] =
                  NexusLlmBridgeStreamState();
            }
            Dict result;
            result.Set("streamId", *stream_id);
            std::move(done).Success(std::move(result));
          },
          weak_factory_.GetWeakPtr(), std::move(done)));
}

void NexusLlmBridgeAdapter::HandleStreamReadOnUI(
    ::nexus::generated::StreamReadInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  if (bridge_stream_states_.find(input.streamId) ==
      bridge_stream_states_.end()) {
    bridge_stream_states_[input.streamId] = NexusLlmBridgeStreamState();
  }

  llm_host_->StreamRead(
      input.streamId,
      base::BindOnce(
          [](base::WeakPtr<NexusLlmBridgeAdapter> self, std::string stream_id,
             ::nexus::BridgeResponseCallback done, bool success,
             mojom::NexusLlmStreamReadResultPtr result,
             std::optional<std::string> error_message) {
            if (!success || !result) {
              std::move(done).Error(
                  "LLM_ERROR",
                  error_message.value_or("StreamRead failed"));
              return;
            }
            if (!self) {
              std::move(done).Error("LLM_ERROR", "Adapter destroyed");
              return;
            }

            auto state_it = self->bridge_stream_states_.find(stream_id);
            if (state_it == self->bridge_stream_states_.end()) {
              state_it = self->bridge_stream_states_
                             .emplace(stream_id, NexusLlmBridgeStreamState())
                             .first;
            }

            bool stream_done = false;
            bool should_cancel = false;
            Dict bridge_result = StreamReadResultToBridge(
                &state_it->second, *result, &stream_done, &should_cancel);

            if (stream_done) {
              self->bridge_stream_states_.erase(state_it);
            }
            if (should_cancel) {
              self->llm_host_->Cancel(
                  stream_id, base::BindOnce(
                                 [](bool /*success*/,
                                    std::optional<std::string> /*error*/) {}));
            }
            std::move(done).Success(std::move(bridge_result));
          },
          weak_factory_.GetWeakPtr(), input.streamId, std::move(done)));
}

void NexusLlmBridgeAdapter::HandleStreamCancelOnUI(
    ::nexus::generated::StreamCancelInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  // Tear down the stream on both sides — see the pre-Phase-10f comment
  // for details. Bridge-side per-stream state cached here also gets
  // dropped — keyed by stream_id and would otherwise leak.
  std::string stream_id = input.streamId;
  // NOTE: do NOT `std::move(stream_id)` into the BindOnce below — `stream_id`
  // is ALSO passed by const-ref as the first argument to `Cancel`, and C++
  // leaves the evaluation order of call arguments unspecified. If the bind
  // (with the move) is built first, the first argument binds to a moved-from
  // (empty) string, so `Cancel` cancels stream id "" — the upstream loader is
  // never torn down and a mid-stream cancel never closes the socket. Pass a
  // copy to the bind so the by-ref argument is always the real id.
  llm_host_->Cancel(
      stream_id,
      base::BindOnce(
          [](base::WeakPtr<NexusLlmBridgeAdapter> self, std::string stream_id,
             ::nexus::BridgeResponseCallback done, bool /*success*/,
             std::optional<std::string> /*error*/) {
            if (self) {
              self->bridge_stream_states_.erase(stream_id);
            }
            std::move(done).SuccessVoid();
          },
          weak_factory_.GetWeakPtr(), stream_id, std::move(done)));
}

void NexusLlmBridgeAdapter::HandleSetDefaultSelectionOnUI(
    ::nexus::generated::SetDefaultSelectionInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  std::string error_message;
  if (!llm_host_->SetDefaultSelection(input.providerId, input.modelId,
                                      &error_message)) {
    std::move(done).Error(
        "LLM_ERROR",
        error_message.empty() ? "Failed to persist default model selection"
                              : error_message);
    return;
  }
  std::move(done).SuccessVoid();
}

void NexusLlmBridgeAdapter::HandlePublishCatalogOnUI(
    ::nexus::generated::PublishCatalogInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  // Payload shape: { catalog: {...} } — the curated NEXUS_MODEL_CONFIG
  // serialized to JSON-friendly form on the TS side.
  const Dict* catalog = input.payload.FindDict("catalog");
  if (!catalog) {
    std::move(done).InvalidArgs("Missing 'catalog' object in payload");
    return;
  }

  LlmBroker* broker = LlmBrokerFactory::GetForProfile(profile_);
  if (!broker) {
    std::move(done).Error("LLM_ERROR", "LlmBroker unavailable");
    return;
  }
  broker->SetTsModelCatalog(catalog->Clone());
  std::move(done).SuccessVoid();
}

void NexusLlmBridgeAdapter::HandlePublishProviderConfigsOnUI(
    ::nexus::generated::PublishProviderConfigsInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  // Payload shape: { configs: {<providerId>: {apiKey,baseUrl,...}},
  // activeProviderId?: string } — the effective provider configs from
  // settings.json the worker pushes at boot + on every providers write.
  const Dict* configs = input.payload.FindDict("configs");
  if (!configs) {
    std::move(done).InvalidArgs("Missing 'configs' object in payload");
    return;
  }
  const std::string* active = input.payload.FindString("activeProviderId");

  LlmBroker* broker = LlmBrokerFactory::GetForProfile(profile_);
  if (!broker) {
    std::move(done).Error("LLM_ERROR", "LlmBroker unavailable");
    return;
  }
  broker->SetProviderConfigs(configs->Clone(),
                             active ? *active : std::string());
  std::move(done).SuccessVoid();
}

void NexusLlmBridgeAdapter::HandleRegisterToolDefsOnUI(
    ::nexus::generated::RegisterToolDefsInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  // Register/overwrite each full tool definition by name so its `{ ref }`
  // resolves to the schema in ConvertBridgeTools (static-JS at session init,
  // MCP on connect). Defs without a name are skipped.
  PromptHolderRegistry& registry = PromptHolderRegistry::Get();
  for (Dict& def : input.defs) {
    const std::string* name = def.FindString("name");
    if (!name || name->empty()) {
      continue;
    }
    std::string name_copy = *name;
    registry.RegisterToolDef(name_copy, std::move(def));
  }
  std::move(done).SuccessVoid();
}

void NexusLlmBridgeAdapter::HandlePublishBinaryRegistryOnUI(
    ::nexus::generated::PublishBinaryRegistryInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  // Payload shape: { binaries: [ { id, label?, version?, assets: [...] } ] }
  // — the managed-binaries.json the guest fetched same-origin. The store
  // validates per-asset and keeps the previous good config on a bad
  // publish, so we don't reject here.
  NexusBinaryConfigRegistry::Get().SetFromDict(input.payload);
  std::move(done).SuccessVoid();
}

// Phase 12f: `HandlePublishStatusOnUI` moved out — the `session`
// capability is now its own codegen base + adapter
// (`api/session/nexus_session_bridge_adapter.cc`). The
// `PushSessionApplySelection` path below STAYS here because it's a
// PUSH-from-host event triggered by an LLM-broker pick observer; the
// session adapter would have to call back into the broker observer
// chain for no benefit.

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
