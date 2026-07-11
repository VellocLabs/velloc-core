#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_NEXUS_LLM_HOST_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_NEXUS_LLM_HOST_H_

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "custom_browser/browser/nexus/api/llm/llm_types.h"
#include "custom_browser/browser/nexus/mojom/nexus_llm.mojom.h"

class PrefService;
class Profile;

namespace nexus {

// Returns the brand SVG for the given provider id (e.g. the `openai`,
// `anthropic`, `kimi`, `glm` etc. icons baked into the browser_webui
// resource bundle), or a generic placeholder badge if `provider_id` is
// unknown or no resource bundle is loaded. Defined in nexus_llm_host.cc;
// thin wrapper around the in-file helper of the same name.
std::string GetNexusProviderIconSvg(const std::string& provider_id);

// True when the user has saved a successful "verify connection" result
// for `provider_id` (i.e. `nexus.provider_configs[provider_id].
// last_verification.verifiedAtMs` is set). The popup uses this to hide
// providers that haven't been confirmed reachable.
bool IsNexusProviderVerified(PrefService* prefs,
                             const std::string& provider_id);

// True when `model_id` is in the user's per-provider `enabled_models`
// allowlist (or the allowlist is empty / unset, which means "show all").
bool IsNexusModelEnabled(PrefService* prefs,
                         const std::string& provider_id,
                         const std::string& model_id);

// Direct bridge LLM host that adapts the browser bridge contract to the Nexus
// broker/provider layer.
class NexusLlmHost {
 public:
  using CatalogCallback =
      base::OnceCallback<void(bool success,
                              mojom::NexusLlmCatalogPtr catalog,
                              std::optional<std::string> error_message)>;
  using CallCallback =
      base::OnceCallback<void(bool success,
                              mojom::NexusLlmCallResultPtr result,
                              std::optional<std::string> error_message)>;
  using StreamStartCallback =
      base::OnceCallback<void(bool success,
                              std::optional<std::string> stream_id,
                              std::optional<std::string> error_message)>;
  using StreamReadCallback =
      base::OnceCallback<void(bool success,
                              mojom::NexusLlmStreamReadResultPtr result,
                              std::optional<std::string> error_message)>;
  using CancelCallback =
      base::OnceCallback<void(bool success,
                              std::optional<std::string> error_message)>;

  explicit NexusLlmHost(Profile* profile);
  ~NexusLlmHost();

  NexusLlmHost(const NexusLlmHost&) = delete;
  NexusLlmHost& operator=(const NexusLlmHost&) = delete;

  void Catalog(CatalogCallback callback);
  void Call(mojom::NexusLlmRequestParamsPtr request,
            CallCallback callback);
  void StreamStart(mojom::NexusLlmRequestParamsPtr request,
                   StreamStartCallback callback);
  void StreamRead(const std::string& stream_id,
                  StreamReadCallback callback);
  void Cancel(const std::string& stream_id, CancelCallback callback);
  bool SetDefaultSelection(const std::string& provider_id,
                           const std::string& model_id,
                           std::string* error_message);

  struct PendingToolCall {
    std::string tool_call_id;
    std::string tool_name;
    std::string args_json;
  };

  struct StreamState {
    StreamState();
    StreamState(const StreamState&) = delete;
    StreamState& operator=(const StreamState&) = delete;
    StreamState(StreamState&&);
    StreamState& operator=(StreamState&&);
    ~StreamState();

    std::string stream_id;
    std::string provider_id;
    std::string model_id;
    std::vector<mojom::NexusLlmStreamEventPtr> queued_events;
    std::map<std::string, PendingToolCall> pending_tool_calls;
    std::vector<std::string> pending_tool_call_order;
    bool terminal_event_queued = false;
    bool done = false;
  };

 private:
  // OAuth-backed providers need a fresh access token before dispatch. These
  // run an async refresh (no-op for non-OAuth providers), apply the token to
  // the provider in-memory, then continue the original Call / StreamStart.
  void EnsureProviderAuthThen(const std::string& provider_id,
                              base::OnceCallback<void(bool ok)> done);
  void OnEnsureFreshToken(const std::string& provider_id,
                          base::OnceCallback<void(bool ok)> done,
                          bool ok,
                          std::string access_token,
                          std::string account_id);
  void ContinueCall(mojom::NexusLlmRequestParamsPtr request,
                    CallCallback callback,
                    bool auth_ok);
  void ContinueStreamStart(mojom::NexusLlmRequestParamsPtr request,
                           StreamStartCallback callback,
                           bool auth_ok);

  void OnGenerateComplete(const std::string& resolved_model_id,
                          CallCallback callback,
                          GenerateResult result,
                          std::optional<std::string> error);
  void OnStreamChunk(const std::string& stream_id, base::DictValue chunk);
  void OnStreamDone(const std::string& stream_id,
                    std::optional<std::string> error);

  raw_ptr<Profile> profile_;
  std::map<std::string, StreamState> streams_;
  base::TimeTicks last_catalog_refresh_time_;
  std::vector<std::string> last_catalog_refresh_provider_ids_;
  base::WeakPtrFactory<NexusLlmHost> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_NEXUS_LLM_HOST_H_
