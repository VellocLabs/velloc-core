#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_OPENAI_OPENAI_PROVIDER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_OPENAI_OPENAI_PROVIDER_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/values.h"
#include "custom_browser/browser/nexus/api/llm/llm_provider.h"
#include "custom_browser/browser/nexus/api/llm/llm_stream_guard.h"
#include "custom_browser/browser/nexus/api/llm/llm_types.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"

namespace network {
class SimpleURLLoader;
struct ResourceRequest;
}  // namespace network

namespace nexus {

class OpenAiProvider : public LlmProvider {
 public:
  explicit OpenAiProvider(
      scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory);

  // Constructor for OpenAI-compatible providers (Kimi, MiniMax, Qwen, GLM,
  // etc.). The OpenAI request/response format, SSE streaming, /models
  // discovery, and tool-calling schema are reused; only provider identity
  // and default base URL differ.
  OpenAiProvider(
      scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
      std::string provider_id,
      std::string display_name,
      std::string default_base_url);

  // Full constructor. `uses_responses_api` selects the /responses endpoint
  // and event-driven SSE parser (vs /chat/completions). `extra_headers` are
  // added to every inference request. Used by the OAuth-backed ChatGPT
  // Plus/Pro provider, which speaks the Responses API against the Codex
  // backend with `originator` / `OpenAI-Beta` headers. `requires_api_key`
  // false lets a keyless proxy (e.g. the user-configured "custom-openai"
  // provider) count as configured with only a base URL.
  OpenAiProvider(
      scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
      std::string provider_id,
      std::string display_name,
      std::string default_base_url,
      bool uses_responses_api,
      std::map<std::string, std::string> extra_headers,
      bool requires_api_key = true);

  ~OpenAiProvider() override;

  OpenAiProvider(const OpenAiProvider&) = delete;
  OpenAiProvider& operator=(const OpenAiProvider&) = delete;

  // LlmProvider:
  std::string GetId() const override;
  std::string GetDisplayName() const override;
  LlmProviderType GetProviderType() const override;
  std::vector<ModelInfo> GetSupportedModels() const override;
  std::string GetDefaultModelId() const override;
  bool SupportsStreaming() const override;
  bool SupportsTools() const override;
  bool IsConfigured() const override;
  bool Configure(const base::DictValue& config) override;
  std::string GetDefaultBaseUrl() const override;
  std::string GetBaseUrl() const override;

  void DoGenerate(const base::DictValue& call_options,
                  GenerateCallback callback) override;
  void DoStream(const base::DictValue& call_options,
                StreamChunkCallback chunk_callback,
                StreamDoneCallback done_callback) override;
  void CancelRequest(const std::string& request_id) override;
  void FetchAvailableModels(FetchModelsCallback callback) override;
  void LoadCachedCatalogJson(const std::string& raw_json) override;
  std::string GetLastFetchedRawCatalogJson() const override;

 private:
  class StreamConsumer;
  struct StreamContext;

  // Adds `extra_headers_` and (when set) the `ChatGPT-Account-Id` header to an
  // outgoing inference request. No-op for providers without either.
  void ApplyExtraHeaders(network::ResourceRequest* request) const;

  base::DictValue BuildRequestBody(const base::DictValue& call_options,
                                     bool stream) const;
  base::ListValue TranslateMessages(const base::ListValue& prompt) const;
  base::DictValue TranslateToolDefinition(
      const base::DictValue& tool) const;
  std::string ResolveModel(const base::DictValue& call_options) const;

  GenerateResult ParseResponse(const base::DictValue& response) const;

  // Responses API path — OpenAI now recommends /v1/responses as the
  // canonical endpoint for text generation (chat-completions remains for
  // backwards compatibility, but reasoning models require this path).
  // Whether a request takes this path is decided by `uses_responses_api_`
  // (true for the native `openai` provider and the OAuth `chatgpt-pro`
  // provider). OpenAI-compatible providers (Kimi, MiniMax, Qwen, GLM, …)
  // stay on /v1/chat/completions because their servers don't implement
  // /responses.
  // True when this provider targets the ChatGPT subscription (Codex) backend
  // (`chatgpt.com/backend-api/codex`), which needs a stricter Responses body
  // than the public /v1/responses API.
  bool IsCodexBackend() const;

  base::DictValue BuildResponsesApiRequestBody(
      const base::DictValue& call_options, bool stream) const;
  base::ListValue TranslateMessagesToResponsesInput(
      const base::ListValue& prompt,
      std::string* out_instructions) const;
  base::DictValue TranslateToolDefinitionForResponses(
      const base::DictValue& tool) const;
  GenerateResult ParseResponsesApiResponse(
      const base::DictValue& response) const;
  void ProcessResponsesApiSseEvent(StreamContext& ctx,
                                    std::string_view event_type,
                                    const base::DictValue& data);

  void OnGenerateComplete(const std::string& request_id,
                          GenerateCallback callback,
                          bool use_responses_api,
                          std::optional<std::string> response_body);
  void OnFetchModelsComplete(FetchModelsCallback callback,
                             std::optional<std::string> response_body);

  // NOTE: `request_id` is taken BY VALUE on purpose. Both methods are invoked
  // by StreamConsumer with its own `request_id_` member as the argument, and
  // both can destroy that consumer mid-call (the chunk/done callback chain or
  // an erase from active_streams_ destroys the StreamContext, which owns the
  // consumer). A const-ref parameter would dangle the moment that happens —
  // this exact bug crashed in the field as an INVALID_POINTER_READ inside
  // memcmp under active_streams_.find(request_id).
  void OnStreamDataReceived(std::string request_id, std::string_view data);
  void OnStreamComplete(std::string request_id, bool success);
  void ProcessSseLine(StreamContext& ctx, std::string_view line);

  // Actually builds + starts the SSE loader for a stream. Called directly
  // from DoStream when a concurrency slot is free, or from PumpQueuedStreams
  // when a queued stream is admitted.
  void StartStream(const std::string& request_id,
                   const base::DictValue& call_options,
                   StreamChunkCallback chunk_callback,
                   StreamDoneCallback done_callback);
  // Guard callback: a tracked stream went silent past the idle timeout.
  void OnStreamIdleTimeout(const std::string& request_id);
  // Admits queued requests (streams and blocking) while a slot is free.
  void PumpQueuedStreams();
  // Actually builds + starts the blocking (DownloadToString) loader. Called
  // directly from DoGenerate when a concurrency slot is free, or from
  // PumpQueuedStreams when a queued blocking request is admitted.
  void StartGenerate(const std::string& request_id,
                     const base::DictValue& call_options,
                     GenerateCallback callback);

  const std::string provider_id_;
  const std::string display_name_;
  const std::string default_base_url_;

  std::string api_key_;
  std::string base_url_;
  std::string configured_model_;
  std::vector<ModelInfo> fetched_models_;
  std::string last_raw_catalog_json_;

  // True → route requests through the OpenAI Responses API (/responses) and
  // its event-driven SSE parser. Set per-provider in the constructor.
  const bool uses_responses_api_;
  // False → this provider may run keyless (custom/proxy endpoints); it counts
  // as configured once a base URL is set, and Configure() tolerates an empty
  // api_key. True for every built-in cloud provider.
  const bool requires_api_key_;
  // Extra headers added to every inference request (e.g. originator,
  // OpenAI-Beta for the ChatGPT Codex backend).
  const std::map<std::string, std::string> extra_headers_;
  // OAuth account id sent as the `ChatGPT-Account-Id` header. Set via
  // Configure(); empty for non-OAuth providers.
  std::string account_id_;

  scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory_;
  std::unique_ptr<network::SimpleURLLoader> models_loader_;
  // Callers that requested a model fetch while one was already in flight;
  // resolved (with the same result) when the active fetch completes. Without
  // this, starting a second fetch destroyed the first loader and silently
  // dropped its callback — hanging NexusLlmHost::Catalog()'s barrier.
  std::vector<FetchModelsCallback> pending_models_callbacks_;

  std::map<std::string, std::unique_ptr<network::SimpleURLLoader>>
      pending_loaders_;
  std::map<std::string, std::unique_ptr<StreamContext>> active_streams_;

  // Shared idle-timeout + concurrency guard (per-provider instance). See
  // llm_stream_guard.h. Defaults preserve prior behaviour (180s idle, 0 =
  // unlimited concurrency); provider config overrides land in Configure().
  LlmStreamGuard stream_guard_;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<OpenAiProvider> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_OPENAI_OPENAI_PROVIDER_H_
