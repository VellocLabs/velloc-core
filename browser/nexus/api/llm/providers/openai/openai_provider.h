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
  // etc.). The /models discovery is reused; only provider identity and
  // default base URL differ. The factory passes an empty default for
  // these — the guest's catalog supplies it (see SetDefaultBaseUrl).
  OpenAiProvider(
      scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
      std::string provider_id,
      std::string display_name,
      std::string default_base_url);

  // Full constructor. `extra_headers` are added to every model-listing
  // request. Used by the OAuth-backed providers: ChatGPT Plus/Pro lists
  // models from the Codex backend with `originator` / `OpenAI-Beta` headers. `requires_api_key`
  // false lets a keyless proxy (e.g. the user-configured "custom-openai"
  // provider) count as configured with only a base URL.
  OpenAiProvider(
      scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
      std::string provider_id,
      std::string display_name,
      std::string default_base_url,
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
  void SetDefaultBaseUrl(const std::string& url) override;
  std::string GetBaseUrl() const override;

  void FetchAvailableModels(FetchModelsCallback callback) override;
  void LoadCachedCatalogJson(const std::string& raw_json) override;
  std::string GetLastFetchedRawCatalogJson() const override;

 private:
  // Adds `extra_headers_` and (when set) the `ChatGPT-Account-Id` header to an
  // outgoing model-listing request. No-op for providers without either.
  void ApplyExtraHeaders(network::ResourceRequest* request) const;

  // True when this provider targets the ChatGPT subscription (Codex) backend
  // (`chatgpt.com/backend-api/codex`), whose model list lives at a different
  // path and shape than the public /v1/models.
  bool IsCodexBackend() const;

  void OnFetchModelsComplete(FetchModelsCallback callback,
                             std::optional<std::string> response_body);

  const std::string provider_id_;
  const std::string display_name_;
  std::string default_base_url_;

  std::string api_key_;
  std::string base_url_;
  // True while `base_url_` is a user override rather than the default.
  bool base_url_overridden_ = false;
  std::string configured_model_;
  std::vector<ModelInfo> fetched_models_;
  std::string last_raw_catalog_json_;

  // False → this provider may run keyless (custom/proxy endpoints); it counts
  // as configured once a base URL is set, and Configure() tolerates an empty
  // api_key. True for every built-in cloud provider.
  const bool requires_api_key_;
  // Extra headers added to every model-listing request (e.g. originator,
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

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<OpenAiProvider> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_OPENAI_OPENAI_PROVIDER_H_
