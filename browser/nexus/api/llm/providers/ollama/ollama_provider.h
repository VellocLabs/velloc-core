#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_OLLAMA_OLLAMA_PROVIDER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_OLLAMA_OLLAMA_PROVIDER_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/values.h"
#include "custom_browser/browser/nexus/api/llm/llm_line_splitter.h"
#include "custom_browser/browser/nexus/api/llm/llm_provider.h"
#include "custom_browser/browser/nexus/api/llm/llm_types.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"

namespace network {
class SimpleURLLoader;
}  // namespace network

namespace nexus {

class OllamaProvider final : public LlmProvider {
 public:
  explicit OllamaProvider(
      scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory);
  ~OllamaProvider() override;

  OllamaProvider(const OllamaProvider&) = delete;
  OllamaProvider& operator=(const OllamaProvider&) = delete;

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

  // Forward-declared so provider-local helpers in the .cc can accept it.
  class StreamBodyConsumer;

  struct StreamContext {
    StreamContext();
    ~StreamContext();

    std::unique_ptr<network::SimpleURLLoader> loader;
    std::unique_ptr<StreamBodyConsumer> consumer;
    StreamChunkCallback chunk_callback;
    StreamDoneCallback done_callback;
    LineSplitter line_splitter;
    std::string id;

    // Track in-flight tool calls by SSE index. Ollama's OpenAI-compatible
    // streaming protocol emits the `id` + `name` only on the first frame of
    // each tool call; subsequent frames carry an `index` plus an `arguments`
    // chunk with no `id`. Without this map we'd lose correlation and each
    // argument chunk would start a brand-new tool call downstream.
    std::map<int, std::string> tool_call_ids;

    // Most Ollama /v1 completions return finish_reason + usage in the same
    // SSE chunk, but some OpenAI-compatible wrappers split them. Buffer both
    // so the finish chunk always carries usage when available.
    std::optional<std::string> pending_finish_reason;
    std::optional<base::DictValue> pending_usage;
    bool finish_emitted = false;
  };

 private:
  struct PendingRequest {
    PendingRequest();
    PendingRequest(std::unique_ptr<network::SimpleURLLoader> loader,
                   std::string id);
    ~PendingRequest();
    PendingRequest(PendingRequest&&);
    PendingRequest& operator=(PendingRequest&&);
    PendingRequest(const PendingRequest&) = delete;
    PendingRequest& operator=(const PendingRequest&) = delete;

    std::unique_ptr<network::SimpleURLLoader> loader;
    std::string id;
  };

  base::DictValue BuildRequestBody(const base::DictValue& call_options,
                                     bool stream) const;
  base::ListValue TranslateMessages(const base::ListValue& prompt) const;
  base::DictValue TranslateToolDefinition(
      const base::DictValue& tool) const;
  std::string ResolveModel(const base::DictValue& call_options) const;

  GenerateResult ParseResponse(const base::DictValue& response) const;

  void OnGenerateComplete(const std::string& request_id,
                          GenerateCallback callback,
                          std::optional<std::string> response_body);
  // NOTE: `request_id` is taken BY VALUE on purpose: the bound argument is
  // owned by the StreamBodyConsumer inside the StreamContext, which these
  // methods can destroy mid-call (see the OpenAI provider's field crash for
  // the dangling const-ref variant of this pattern).
  void OnStreamDataReceived(std::string request_id,
                            std::string_view string_piece);
  void OnStreamComplete(std::string request_id, bool success);
  void ProcessNdjsonLine(StreamContext& ctx, const std::string& line);

  void OnFetchModelsComplete(FetchModelsCallback callback,
                             std::optional<std::string> response_body);

  std::string base_url_;
  std::string configured_model_;
  std::vector<ModelInfo> fetched_models_;
  std::string last_raw_catalog_json_;

  scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory_;
  std::unique_ptr<network::SimpleURLLoader> models_loader_;
  // Fetch requests that arrived while models_loader_ was in flight; resolved
  // with the active fetch's result (see FetchAvailableModels).
  std::vector<FetchModelsCallback> pending_models_callbacks_;

  std::map<std::string, PendingRequest> pending_requests_;
  std::map<std::string, std::unique_ptr<StreamContext>> active_streams_;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<OllamaProvider> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_OLLAMA_OLLAMA_PROVIDER_H_
