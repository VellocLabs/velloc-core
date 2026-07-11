#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_LLM_PROVIDERS_OPENAI_OPENAI_PROVIDER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_LLM_PROVIDERS_OPENAI_OPENAI_PROVIDER_H_

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
#include "custom_browser/browser/nexus/llm/llm_provider.h"
#include "custom_browser/browser/nexus/llm/llm_types.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"

namespace network {
class SimpleURLLoader;
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
  void OnFetchModelsComplete(FetchModelsCallback callback,
                             std::optional<std::string> response_body);

  void OnStreamDataReceived(const std::string& request_id,
                            std::string_view data);
  void OnStreamComplete(const std::string& request_id, bool success);
  void ProcessSseLine(StreamContext& ctx, std::string_view line);

  const std::string provider_id_;
  const std::string display_name_;
  const std::string default_base_url_;

  std::string api_key_;
  std::string base_url_;
  std::string configured_model_;
  std::vector<ModelInfo> fetched_models_;
  std::string last_raw_catalog_json_;

  scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory_;
  std::unique_ptr<network::SimpleURLLoader> models_loader_;

  std::map<std::string, std::unique_ptr<network::SimpleURLLoader>>
      pending_loaders_;
  std::map<std::string, std::unique_ptr<StreamContext>> active_streams_;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<OpenAiProvider> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_LLM_PROVIDERS_OPENAI_OPENAI_PROVIDER_H_
