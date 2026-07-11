#include "custom_browser/browser/nexus/llm/nexus_llm_bridge_adapter.h"

#include <utility>

#include "base/functional/bind.h"
#include "chrome/browser/profiles/profile.h"
#include "custom_browser/browser/nexus/llm/nexus_llm_host.h"

namespace nexus {

namespace {

using base::Value;
using Dict = base::DictValue;

}  // namespace

NexusLlmBridgeAdapter::NexusLlmBridgeAdapter(
    NexusBridgeResponseDelegate* delegate,
    Profile* profile)
    : NexusBridgeCapabilityHandler(delegate),
      llm_host_(std::make_unique<NexusLlmHost>(profile)) {
  Register("catalog", &NexusLlmBridgeAdapter::HandleCatalog);
  Register("call", &NexusLlmBridgeAdapter::HandleCall);
  Register("setDefaultSelection", "providerId", "modelId",
           &NexusLlmBridgeAdapter::HandleSetDefaultSelection);
  Register("streamStart", &NexusLlmBridgeAdapter::HandleStreamStart);
  Register("streamRead", "streamId", &NexusLlmBridgeAdapter::HandleStreamRead);
}

NexusLlmBridgeAdapter::~NexusLlmBridgeAdapter() = default;

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

  llm_host_->Call(
      std::move(request),
      base::BindOnce(
          [](base::WeakPtr<NexusLlmBridgeAdapter> self, std::string id,
             Dict route, bool success, mojom::NexusLlmCallResultPtr result,
             std::optional<std::string> error_message) {
            if (!self) {
              return;
            }
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

}  // namespace nexus
