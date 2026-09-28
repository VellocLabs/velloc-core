#include "custom_browser/browser/nexus/api/llm/nexus_llm_bridge_adapter.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/time/time.h"
#include "chrome/browser/profiles/profile.h"
#include "custom_browser/browser/nexus/api/catalog/prompt_holder_registry.h"
#include "custom_browser/browser/nexus/binaries/nexus_binary_config_registry.h"
#include "custom_browser/browser/nexus/api/llm/llm_broker.h"
#include "custom_browser/browser/nexus/api/llm/llm_broker_factory.h"
#include "custom_browser/browser/nexus/api/llm/nexus_llm_host.h"
#include "custom_browser/browser/nexus/nexus_secret_crypt.h"
#include "base/containers/span.h"
#include "base/strings/string_util.h"
#include "base/task/sequenced_task_runner.h"
#include "base/uuid.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "content/public/browser/storage_partition.h"
#include "custom_browser/browser/nexus/api/llm/llm_provider.h"
#include "custom_browser/browser/nexus/api/llm/llm_pref_names.h"
#include "custom_browser/browser/nexus/api/llm/llm_raw_stream_policy.h"
#include "custom_browser/browser/nexus/api/llm/provider_config.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "net/base/url_util.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_util.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "url/gurl.h"

namespace nexus {

namespace {

using base::Value;
using Dict = base::DictValue;

// Wall-clock epoch-ms for the bridge timeline stamps (t_recv_native). Uses
// base::Time so the stamps line up with JS-side Date.now() values.
double NowEpochMs() {
  return static_cast<double>(
      base::Time::Now().InMillisecondsFSinceUnixEpochIgnoringNull());
}

// Raw provider streams (`model.rawStream*`). A session drives one at a time;
// the cap only bounds a runaway or hostile guest.
constexpr size_t kMaxRawStreams = 16;
constexpr size_t kMaxRawStreamUrlChars = 8192;
constexpr int kDefaultRawStreamIdleTimeoutMs = 90'000;
constexpr size_t kDefaultRawStreamMaxResponseBytes = 200'000'000;
// A secret-capturing response is buffered whole; token responses are small.
constexpr size_t kMaxCaptureResponseBytes = 1024 * 1024;
// The secret name that addresses the provider's API key (`api_key` in the
// stored config); every other name is an entry of the config's `secrets`.
constexpr char kApiKeySecretName[] = "api_key";
constexpr int kDefaultRawStreamReadMaxBytes = 1024 * 1024;

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

  // Native deadline contract: the guest stamps `deadlineMs` at the top of
  // the passthrough payload (absolute epoch-ms = JS t0 + budget). When the
  // deadline fires before the provider answers, the host cancels the
  // upstream request via broker->CancelRequest and resolves with
  // status:'timeout' — see NexusLlmHost::OnCallDeadlineTimeout.
  std::optional<double> deadline_ms;
  if (std::optional<double> v = input.payload.FindDouble("deadlineMs")) {
    deadline_ms = *v;
  }
  Dict timeline;
  timeline.Set("t_recv_native", NowEpochMs());

  llm_host_->Call(
      std::move(request), deadline_ms, std::move(timeline),
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
  // NOTE: the per-stream bridge state is created lazily in the completion
  // callback below, and only for reads the host answered successfully.
  // Pre-creating it here leaked one NexusLlmBridgeStreamState per unknown /
  // guest-invented stream id (the host errors and the error path returned
  // without erasing) — an unbounded map for a hostile or buggy guest.

  // Hold-read contract: when the guest supplies `waitMs` / `deadlineMs`,
  // the host may hold this read (queue empty, stream not done) until fresh
  // data arrives, the stream finishes, or the hold window expires — see
  // NexusLlmHost::StreamRead. A native timeout ALSO cancels the upstream
  // loader, so the guest can stop polling without an explicit streamCancel.
  Dict timeline;
  timeline.Set("t_recv_native", NowEpochMs());

  llm_host_->StreamRead(
      input.streamId, input.deadlineMs, input.waitMs, std::move(timeline),
      base::BindOnce(
          [](base::WeakPtr<NexusLlmBridgeAdapter> self, std::string stream_id,
             ::nexus::BridgeResponseCallback done, bool success,
             mojom::NexusLlmStreamReadResultPtr result,
             std::optional<std::string> error_message) {
            if (!success || !result) {
              if (self) {
                // Drop any bridge-side state for a stream the host no longer
                // recognizes — it would otherwise leak per errored read.
                self->bridge_stream_states_.erase(stream_id);
              }
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

            // On a native hold-read timeout the host has already cancelled
            // the upstream loader and erased ITS stream entry, and the guest
            // abandons the stream (no further read, no cancel). Mirror that
            // here or the bridge state leaks once per timed-out stream.
            const bool host_timed_out = result->status == "timeout";
            if (stream_done || host_timed_out) {
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

void NexusLlmBridgeAdapter::HandleEncryptSecretOnUI(
    ::nexus::generated::EncryptSecretInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  // The plaintext crosses the bridge exactly once — here, on save. The
  // guest stores the sealed value in settings.json and republishes it to
  // `publishProviderConfigs`, where `LlmProvider::Configure` decrypts it
  // natively through the memoized NexusSecretCrypt cache. Nothing on this
  // route ever returns plaintext to the guest.
  //
  // The encryptor is primed by `NexusBridgePortService`'s ctor (long before
  // any bridge request can arrive), so `WhenReady` resolves immediately in
  // practice; it is used rather than `Encrypt`'s not-ready fallback because
  // that fallback returns the PLAINTEXT, and this route must never do that.
  NexusSecretCrypt* crypt = NexusSecretCrypt::GetInstance();
  crypt->WhenReady(base::BindOnce(
      [](std::string plaintext, ::nexus::BridgeResponseCallback done) {
        const std::string sealed =
            NexusSecretCrypt::GetInstance()->Encrypt(plaintext);
        // Encrypt returns the input unchanged on an empty secret (nothing
        // to seal — reported back as-is) and on an OS-crypto failure, which
        // must surface as an error rather than as a "sealed" plaintext.
        if (!plaintext.empty() && !NexusSecretCrypt::IsEncrypted(sealed)) {
          std::move(done).Error("ENCRYPT_FAILED",
                                "The OS keystore could not seal the secret");
          return;
        }
        base::DictValue result;
        result.Set("value", sealed);
        std::move(done).Success(std::move(result));
      },
      std::move(input.plaintext), std::move(done)));
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

void NexusLlmBridgeAdapter::HandleRawStreamOpenOnUI(
    ::nexus::generated::RawStreamOpenInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  if (!profile_) {
    std::move(done).Error("LLM_ERROR", "No profile.");
    return;
  }
  if (raw_streams_.size() >= kMaxRawStreams) {
    std::move(done).Error("TOO_MANY_STREAMS",
                          "Too many raw streams are open.");
    return;
  }

  // Base URL: the live provider's active URL (user override, else the
  // guest-published default). A provider whose dialect the host does not
  // implement natively (its wire format lives in the guest) has no broker
  // instance — fall back to the base URL stored in its config.
  const base::DictValue* config =
      GetStoredProviderConfig(profile_->GetPrefs(), input.providerId);
  LlmBroker* broker = LlmBrokerFactory::GetForProfile(profile_);
  LlmProvider* provider =
      broker ? broker->GetProvider(input.providerId) : nullptr;
  if (!provider && !config) {
    std::move(done).Error("PROVIDER_UNKNOWN",
                          "Unknown provider: " + input.providerId);
    return;
  }
  std::string base_url;
  if (provider) {
    base_url = provider->GetBaseUrl();
  } else if (const std::string* stored = config->FindString("base_url")) {
    base_url = *stored;
  }
  const GURL base(base_url);

  if (input.url.size() > kMaxRawStreamUrlChars) {
    std::move(done).InvalidArgs("url is too long.");
    return;
  }
  GURL target(input.url);
  if (std::optional<std::string> error = CheckRawStreamTarget(base, target)) {
    std::move(done).Error("TARGET_NOT_ALLOWED", *error);
    return;
  }

  // Secrets are read and decrypted here, host-side, and only ever written
  // into the outgoing request — never returned to the guest.
  const SecretLookup lookup = base::BindRepeating(
      [](const base::DictValue* config,
         const std::string& name) -> std::optional<std::string> {
        if (!config) {
          return std::nullopt;
        }
        const std::string* stored = nullptr;
        if (name == kApiKeySecretName) {
          stored = config->FindString("api_key");
        } else if (const base::DictValue* secrets =
                       config->FindDict("secrets")) {
          stored = secrets->FindString(name);
        }
        if (!stored) {
          return std::nullopt;
        }
        return NexusSecretCrypt::GetInstance()->Decrypt(*stored);
      },
      base::Unretained(config));

  // Query parameters are appended AFTER the domain check, so a template can
  // only ever add to the query of an already-approved URL.
  if (input.queryParams.has_value()) {
    for (const auto [name, value] : *input.queryParams) {
      if (!value.is_string()) {
        continue;
      }
      std::string expanded;
      if (std::optional<std::string> error =
              ExpandSecretTemplates(value.GetString(), lookup, &expanded)) {
        std::move(done).Error("SECRET_UNAVAILABLE", *error);
        return;
      }
      target = net::AppendQueryParameter(target, name, expanded);
    }
  }

  auto request = std::make_unique<network::ResourceRequest>();
  request->url = target;
  request->method = input.method.value_or("POST");
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  std::string content_type = "application/octet-stream";
  // Headers a secret was substituted into — dropped on a redirect that
  // leaves the provider's domain.
  std::vector<std::string> secret_headers;
  if (input.headers.has_value()) {
    for (const auto [name, value] : *input.headers) {
      if (!value.is_string()) {
        continue;
      }
      if (IsForbiddenRawStreamHeader(name) ||
          !net::HttpUtil::IsValidHeaderName(name)) {
        std::move(done).InvalidArgs("Header not allowed: " + name);
        return;
      }
      std::string expanded;
      if (std::optional<std::string> error =
              ExpandSecretTemplates(value.GetString(), lookup, &expanded)) {
        std::move(done).Error("SECRET_UNAVAILABLE", *error);
        return;
      }
      // Validated after expansion, so a secret with a CR/LF cannot split
      // the header block.
      if (!net::HttpUtil::IsValidHeaderValue(expanded)) {
        std::move(done).InvalidArgs("Invalid header value: " + name);
        return;
      }
      if (base::EqualsCaseInsensitiveASCII(
              name, net::HttpRequestHeaders::kContentType)) {
        content_type = std::move(expanded);
        continue;  // AttachStringForUpload sets it.
      }
      if (expanded != value.GetString()) {
        secret_headers.push_back(name);
      }
      request->headers.SetHeader(name, expanded);
    }
  }
  if (!request->headers.HasHeader(net::HttpRequestHeaders::kUserAgent)) {
    // The guest's own User-Agent header wins; else the provider's preset.
    std::string user_agent = provider ? provider->user_agent() : std::string();
    if (user_agent.empty() && config) {
      if (const std::string* stored = config->FindString("user_agent")) {
        user_agent = *stored;
      }
    }
    if (!user_agent.empty()) {
      request->headers.SetHeader(net::HttpRequestHeaders::kUserAgent,
                                 user_agent);
    }
  }

  // Secret capture: declared fields of a JSON response are stored as
  // provider secrets and redacted before the guest sees the body.
  std::vector<SecretCapture> captures;
  if (input.captureSecrets.has_value()) {
    for (const base::DictValue& entry : *input.captureSecrets) {
      const std::string* pointer = entry.FindString("pointer");
      const std::string* secret = entry.FindString("secret");
      if (!pointer || pointer->empty() || (*pointer)[0] != '/' || !secret ||
          !IsValidSecretName(*secret) || *secret == kApiKeySecretName) {
        std::move(done).InvalidArgs(
            "captureSecrets entries need a JSON `pointer` and a `secret` "
            "name other than api_key.");
        return;
      }
      captures.push_back({*pointer, *secret});
    }
  }
  if (!captures.empty() && !NexusSecretCrypt::GetInstance()->ready()) {
    std::move(done).Error("SECRET_UNAVAILABLE",
                          "The OS keystore is not ready; cannot store "
                          "captured secrets.");
    return;
  }

  const int idle_ms = input.idleTimeoutMs.value_or(0) > 0
                          ? *input.idleTimeoutMs
                          : kDefaultRawStreamIdleTimeoutMs;
  size_t max_bytes = input.maxResponseBytes.value_or(0) > 0
                         ? static_cast<size_t>(*input.maxResponseBytes)
                         : kDefaultRawStreamMaxResponseBytes;
  NexusLlmRawStream::BodyTransform transform;
  if (!captures.empty()) {
    max_bytes = std::min(max_bytes, kMaxCaptureResponseBytes);
    // Unretained: the stream (and so this transform) is owned by
    // raw_streams_ and dies with the adapter.
    transform = base::BindOnce(&NexusLlmBridgeAdapter::CaptureSecrets,
                               base::Unretained(this), input.providerId,
                               std::move(captures));
  }

  auto stream = std::make_unique<NexusLlmRawStream>(
      profile_->GetDefaultStoragePartition()
          ->GetURLLoaderFactoryForBrowserProcess(),
      std::move(request), std::string(input.body.begin(), input.body.end()),
      std::move(content_type), max_bytes, base::Milliseconds(idle_ms),
      base::BindRepeating(
          [](const GURL& base, const GURL& next) {
            return !CheckRawStreamTarget(base, next).has_value();
          },
          base),
      std::move(secret_headers), std::move(transform));
  const std::string stream_id =
      base::Uuid::GenerateRandomV4().AsLowercaseString();
  NexusLlmRawStream* stream_ptr = stream.get();
  raw_streams_[stream_id] = std::move(stream);
  stream_ptr->Start(base::BindOnce(&NexusLlmBridgeAdapter::OnRawStreamOpened,
                                   weak_factory_.GetWeakPtr(), stream_id,
                                   std::move(done)));
}

NexusLlmRawStream::TransformResult NexusLlmBridgeAdapter::CaptureSecrets(
    const std::string& provider_id,
    const std::vector<SecretCapture>& captures,
    std::string body,
    int status) {
  NexusLlmRawStream::TransformResult result;
  std::map<std::string, std::string> captured;
  std::optional<std::string> redacted =
      ApplySecretCaptures(body, captures, &captured);
  if (!redacted) {
    if (status >= 200 && status < 300) {
      // A success body we cannot parse may still hold the secret in a shape
      // we did not expect: withhold it rather than hand it to the guest.
      result.error_code = "CAPTURE_FAILED";
      result.error_message = "Response to a secret-capturing request is not "
                             "JSON; body withheld.";
      return result;
    }
    // An error response (plain-text 4xx/5xx) carries no token to capture.
    result.body = std::move(body);
    return result;
  }
  result.body = std::move(*redacted);

  base::DictValue sealed;
  if (!captured.empty() && profile_) {
    ScopedDictPrefUpdate update(profile_->GetPrefs(),
                                prefs::kNexusProviderConfigs);
    base::DictValue* config = update->EnsureDict(provider_id);
    base::DictValue* secrets = config->EnsureDict("secrets");
    for (const auto& [name, plaintext] : captured) {
      std::string value = NexusSecretCrypt::GetInstance()->Encrypt(plaintext);
      secrets->Set(name, value);
      sealed.Set(name, std::move(value));
    }
  }
  // Sealed (OS-keystore ciphertext) copies, so the guest can persist them in
  // settings.json the way it persists a sealed API key.
  result.extra.Set("captured", std::move(sealed));
  return result;
}

void NexusLlmBridgeAdapter::OnRawStreamOpened(
    std::string stream_id,
    ::nexus::BridgeResponseCallback done,
    NexusLlmRawStream::OpenResult result) {
  if (!result.error_code.empty()) {
    EraseRawStreamSoon(stream_id);
    std::move(done).Error(std::move(result.error_code),
                          std::move(result.error_message));
    return;
  }
  Dict out;
  out.Set("streamId", stream_id);
  out.Set("status", result.status);
  out.Set("statusText", std::move(result.status_text));
  out.Set("finalUrl", std::move(result.final_url));
  out.Set("headers", std::move(result.headers));
  std::move(done).Success(std::move(out));
}

void NexusLlmBridgeAdapter::HandleRawStreamReadOnUI(
    ::nexus::generated::RawStreamReadInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  auto it = raw_streams_.find(input.streamId);
  if (it == raw_streams_.end()) {
    std::move(done).Error("STREAM_UNKNOWN",
                          "Unknown raw stream: " + input.streamId);
    return;
  }
  const int max_bytes = input.maxBytes.value_or(0) > 0
                            ? *input.maxBytes
                            : kDefaultRawStreamReadMaxBytes;
  it->second->Read(
      base::Milliseconds(std::max(0, input.waitMs.value_or(0))),
      static_cast<size_t>(max_bytes),
      base::BindOnce(
          [](base::WeakPtr<NexusLlmBridgeAdapter> self, std::string stream_id,
             ::nexus::BridgeResponseCallback done,
             NexusLlmRawStream::ReadResult result) {
            if (result.done && self) {
              self->EraseRawStreamSoon(stream_id);
            }
            Dict envelope = std::move(result.extra);
            envelope.Set("done", result.done);
            if (!result.error_code.empty()) {
              envelope.Set("errorCode", std::move(result.error_code));
              envelope.Set("errorMessage", std::move(result.error_message));
            }
            std::move(done).Binary(
                Value(std::move(envelope)),
                mojo_base::BigBuffer(base::as_byte_span(result.data)),
                /*binary_field=*/"chunk");
          },
          weak_factory_.GetWeakPtr(), input.streamId, std::move(done)));
}

void NexusLlmBridgeAdapter::HandleRawStreamCancelOnUI(
    ::nexus::generated::RawStreamCancelInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  // Destroying the stream cancels its loader and answers any held read.
  raw_streams_.erase(input.streamId);
  std::move(done).SuccessVoid();
}

void NexusLlmBridgeAdapter::EraseRawStreamSoon(const std::string& stream_id) {
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<NexusLlmBridgeAdapter> self, std::string id) {
            if (self) {
              self->raw_streams_.erase(id);
            }
          },
          weak_factory_.GetWeakPtr(), stream_id));
}

}  // namespace nexus
