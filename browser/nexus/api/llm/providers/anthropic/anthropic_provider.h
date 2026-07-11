#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_ANTHROPIC_ANTHROPIC_PROVIDER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_ANTHROPIC_ANTHROPIC_PROVIDER_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/values.h"
#include "custom_browser/browser/nexus/api/llm/llm_provider.h"
#include "custom_browser/browser/nexus/api/llm/llm_types.h"

namespace network {
struct ResourceRequest;
class SharedURLLoaderFactory;
class SimpleURLLoader;
}  // namespace network

namespace nexus {

class AnthropicProvider : public LlmProvider {
 public:
  explicit AnthropicProvider(
      scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory);
  ~AnthropicProvider() override;

  AnthropicProvider(const AnthropicProvider&) = delete;
  AnthropicProvider& operator=(const AnthropicProvider&) = delete;

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
  class StreamSession;
  struct PendingGenerate;

  base::DictValue BuildRequestBody(const base::DictValue& call_options,
                                     bool stream) const;
  base::ListValue TranslateMessages(const base::ListValue& messages,
                                      std::string& system_prompt) const;
  base::ListValue TranslateContentParts(const base::ListValue& parts,
                                          std::string_view role) const;
  base::ListValue TranslateTools(const base::ListValue& tools) const;
  base::DictValue TranslateToolChoice(
      const base::DictValue& choice) const;
  GenerateResult ParseResponse(const base::DictValue& response) const;

  std::unique_ptr<network::ResourceRequest> CreateResourceRequest() const;
  std::string GenerateRequestId();

  void OnGenerateComplete(const std::string& request_id,
                          std::optional<std::string> response_body);
  void OnStreamComplete(const std::string& request_id);
  void OnFetchModelsComplete(FetchModelsCallback callback,
                             std::optional<std::string> response_body);

  std::string api_key_;
  std::string base_url_;
  std::string model_id_;
  scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory_;

  std::map<std::string, std::unique_ptr<PendingGenerate>> pending_generates_;
  std::map<std::string, std::unique_ptr<StreamSession>> pending_streams_;

  std::vector<ModelInfo> fetched_models_;
  std::string last_raw_catalog_json_;
  std::unique_ptr<network::SimpleURLLoader> models_loader_;

  int next_request_id_ = 0;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<AnthropicProvider> weak_ptr_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_ANTHROPIC_ANTHROPIC_PROVIDER_H_
