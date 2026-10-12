#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_PROVIDER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_PROVIDER_H_

#include <memory>
#include <string>
#include <vector>

#include "base/values.h"
#include "custom_browser/browser/nexus/api/llm/llm_types.h"

namespace net {
class HttpRequestHeaders;
}  // namespace net

namespace nexus {

// Abstract base class for all LLM providers: a provider's identity, stored
// configuration (API key, base URL, user agent) and model discovery. The
// runtime speaks every provider's wire format itself over `model.rawStream*`;
// the C++ inference half (DoGenerate / DoStream behind model.call /
// streamStart) was removed under interface 1. The instances
// remain for model listing, key verification (`providerAuth.verify` fetches
// the model list) and as the rawStream base-URL / credential anchor.
//
// Lifecycle:
//   1. Constructed by LlmProviderFactory with a SharedURLLoaderFactory.
//   2. Configure() is called with API key / base URL from prefs.
//   3. FetchAvailableModels() is called to populate the dynamic model list.
class LlmProvider {
 public:
  virtual ~LlmProvider() = default;

  LlmProvider(const LlmProvider&) = delete;
  LlmProvider& operator=(const LlmProvider&) = delete;

  // --- Identity ---
  virtual std::string GetId() const = 0;
  virtual std::string GetDisplayName() const = 0;
  virtual LlmProviderType GetProviderType() const = 0;

  // --- Model catalogue ---

  // Returns the current model list. If a catalogue has been loaded (either
  // via FetchAvailableModels or LoadCachedCatalogJson) this returns that
  // list; otherwise returns an empty vector.
  virtual std::vector<ModelInfo> GetSupportedModels() const = 0;

  virtual std::string GetDefaultModelId() const = 0;

  // Asynchronously fetches the model catalogue from the provider's API
  // (e.g. GET /v1/models). On success the fetched list replaces whatever
  // was previously loaded. On failure the callback receives an empty list.
  // Default implementation invokes |callback| with GetSupportedModels().
  virtual void FetchAvailableModels(FetchModelsCallback callback);

  // Re-populates the in-memory model list from a raw /v1/models response
  // body that was persisted by LlmBroker on a previous run. No network
  // activity. Retained so Settings-UI-triggered refreshes can still round-
  // trip a fetched catalogue through prefs; the authoritative model list
  // shown to users lives in the TS config, not here. Default is a no-op.
  virtual void LoadCachedCatalogJson(const std::string& raw_json);

  // Returns the raw response body from the most recent successful
  // FetchAvailableModels call, or the cached JSON if LoadCachedCatalogJson
  // was the last thing that populated the model list. Empty string if
  // neither has happened yet.
  virtual std::string GetLastFetchedRawCatalogJson() const;

  // --- Capabilities ---
  virtual bool SupportsStreaming() const = 0;
  virtual bool SupportsTools() const = 0;

  // --- Configuration ---
  virtual bool IsConfigured() const = 0;
  virtual bool Configure(const base::DictValue& config) = 0;

  // Returns the provider's default base URL. API-key vendors compile none in:
  // the guest owns them (`defaultBaseUrl` on each provider of the published
  // model catalog) and the broker applies them through SetDefaultBaseUrl(),
  // so this is empty until the catalog arrives. OAuth backends keep the one
  // their OAuth config names; custom endpoints have none.
  virtual std::string GetDefaultBaseUrl() const = 0;

  // Replaces the default base URL. The active base URL follows it unless a
  // user override (a non-empty `base_url` in Configure()) is in effect.
  virtual void SetDefaultBaseUrl(const std::string& url) = 0;

  // Returns the currently active base URL (custom if set, otherwise default).
  virtual std::string GetBaseUrl() const = 0;

  // The User-Agent this provider pins on its HTTP requests, or empty for the
  // host's default. Set by ConfigureUserAgent().
  const std::string& user_agent() const { return user_agent_; }

 protected:
  LlmProvider() = default;

  // Reads the optional `user_agent` key of a Configure() dict — a code-owned
  // per-provider preset the guest publishes as `userAgent` (e.g. OpenCode
  // Zen's `opencode/<version>`). Non-empty pins it; empty or absent restores
  // the host default. Every Configure() calls this first, before any early
  // return, so a config never leaves a previous one's User-Agent behind.
  void ConfigureUserAgent(const base::DictValue& config);

  // Sets the pinned User-Agent on an outgoing request's |headers|, replacing
  // the host default the network stack would add. No-op when none is pinned.
  // Every request a provider sends (model discovery) goes through this.
  void ApplyUserAgent(net::HttpRequestHeaders& headers) const;

 private:
  std::string user_agent_;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_PROVIDER_H_
