#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_NEXUS_LLM_HOST_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_NEXUS_LLM_HOST_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/gtest_prod_util.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/values.h"
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
  // `deadline_ms` is an absolute epoch-ms deadline for the whole call; when
  // present, the host starts a one-shot timer that, on expiry before
  // OnGenerateComplete, cancels the upstream request via
  // LlmBroker::CancelRequest and resolves the callback with a
  // `status:'timeout'` result. `timeline` carries the caller-captured
  // t_recv_native stamp; the host fills t_host_read / t_resolve and returns
  // it on the result.
  void Call(mojom::NexusLlmRequestParamsPtr request,
            std::optional<double> deadline_ms,
            base::DictValue timeline,
            CallCallback callback);
  void StreamStart(mojom::NexusLlmRequestParamsPtr request,
                   StreamStartCallback callback);
  // `deadline_ms` / `wait_ms` implement the "hold-read" contract:
  //   - queue non-empty OR stream done  → resolve immediately (legacy).
  //   - queue empty AND (deadline_ms OR wait_ms) present → HOLD the read:
  //       the callback is stored on the stream and a one-shot timer starts
  //       for `min(wait_ms, deadline_ms - now)`. OnStreamChunk/OnStreamDone
  //       resolve the pending read when fresh data arrives. On timer expiry
  //       with no new data, the host resolves with `status:'timeout'` AND
  //       calls broker->CancelRequest(stream_id) so the upstream loader /
  //       socket is torn down server-side.
  // `timeline` carries the caller-captured t_recv_native stamp; the host
  // fills t_host_read / t_resolve and returns it on the result.
  void StreamRead(const std::string& stream_id,
                  std::optional<double> deadline_ms,
                  std::optional<double> wait_ms,
                  base::DictValue timeline,
                  StreamReadCallback callback);
  void Cancel(const std::string& stream_id, CancelCallback callback);
  bool SetDefaultSelection(const std::string& provider_id,
                           const std::string& model_id,
                           std::string* error_message);

  // Observes every upstream cancel the host issues through
  // CancelUpstreamRequest (currently the OnStreamChunk error/backpressure
  // branches). Runs even when no broker exists (null profile), so unit tests
  // can pin the cancel without a Profile/KeyedService harness — and can
  // simulate the providers' synchronous done-callback re-entry from inside
  // the hook.
  void SetUpstreamCancelHookForTesting(
      base::RepeatingCallback<void(const std::string&)> hook) {
    upstream_cancel_hook_for_testing_ = std::move(hook);
  }

  struct PendingToolCall {
    std::string tool_call_id;
    std::string tool_name;
    std::string args_json;
  };

  // One held streamRead (the JS guest is waiting for new data). Bound into
  // StreamState so OnStreamChunk / OnStreamDone can resolve it and so the
  // hold timer can fire a timeout. The timer is a unique_ptr because
  // base::OneShotTimer is not movable and StreamState needs to stay movable
  // (it's emplaced into streams_ by value).
  struct PendingStreamRead {
    PendingStreamRead();
    PendingStreamRead(const PendingStreamRead&) = delete;
    PendingStreamRead& operator=(const PendingStreamRead&) = delete;
    PendingStreamRead(PendingStreamRead&&);
    PendingStreamRead& operator=(PendingStreamRead&&);
    ~PendingStreamRead();

    StreamReadCallback callback;
    base::DictValue timeline;
    std::unique_ptr<base::OneShotTimer> hold_timer;
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
    // Held streamRead waiting for data; nullopt when no read is pending.
    std::optional<PendingStreamRead> pending_read;
    // Opt-in wire observation (llm_observation.h) parsed off the provider's
    // `{"type":"observation"}` chunk. Set at most once per stream and MOVED
    // out onto the first streamRead result that follows, so the guest sees it
    // exactly once. Stays empty for every request that did not opt in.
    std::optional<LlmObservation> observation;
  };

 private:
  FRIEND_TEST_ALL_PREFIXES(NexusLlmHostTest,
                           StreamErrorChunkAcceptsStringErrorPayload);
  FRIEND_TEST_ALL_PREFIXES(NexusLlmHostTest,
                           StreamErrorChunkCancelsUpstreamRequest);
  FRIEND_TEST_ALL_PREFIXES(NexusLlmHostHoldReadTest,
                           OnStreamDoneErasesEntryAfterResolvingHeldRead);
  FRIEND_TEST_ALL_PREFIXES(NexusLlmHostHoldReadTest,
                           HoldTimeoutErasesEntryAndCancelsUpstream);
  FRIEND_TEST_ALL_PREFIXES(NexusLlmHostHoldReadTest,
                           StreamErrorCancelsUpstreamAndResolvesHeldRead);

  // One held Call waiting on its provider result. Keyed by request_id so
  // OnGenerateComplete can find and cancel the deadline timer before
  // resolving.
  struct PendingCall {
    PendingCall();
    PendingCall(const PendingCall&) = delete;
    PendingCall& operator=(const PendingCall&) = delete;
    PendingCall(PendingCall&&);
    PendingCall& operator=(PendingCall&&);
    ~PendingCall();

    std::string resolved_model_id;
    CallCallback callback;
    base::DictValue timeline;
    std::unique_ptr<base::OneShotTimer> deadline_timer;
  };

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
                    std::optional<double> deadline_ms,
                    base::DictValue timeline,
                    CallCallback callback,
                    bool auth_ok);
  void ContinueStreamStart(mojom::NexusLlmRequestParamsPtr request,
                           StreamStartCallback callback,
                           bool auth_ok);

  void OnGenerateComplete(const std::string& request_id,
                          GenerateResult result,
                          std::optional<std::string> error);
  void OnStreamChunk(const std::string& stream_id, base::DictValue chunk);
  void OnStreamDone(const std::string& stream_id,
                    std::optional<std::string> error);

  // Hold-read machinery. ResolveHeldRead pulls the pending read off `state`
  // (if any), stops its timer, builds a result from the current queue/done
  // and runs the callback. Returns true when a read was resolved.
  bool ResolveHeldRead(StreamState& state, const char* status);
  void OnStreamReadHoldTimeout(const std::string& stream_id);

  // Cancels the upstream provider request (broker->CancelRequest) and
  // notifies the test hook. Provider CancelRequest implementations run the
  // stream's done callback SYNCHRONOUSLY with "Cancelled" (e.g.
  // OpenAiProvider::CancelRequest), re-entering OnStreamDone — so callers
  // must queue their own terminal event first and must not touch StreamState
  // references (or `streams_` iterators) after this returns.
  void CancelUpstreamRequest(const std::string& request_id);

  // Call deadline machinery.
  void OnCallDeadlineTimeout(const std::string& request_id);

  raw_ptr<Profile> profile_;
  base::RepeatingCallback<void(const std::string&)>
      upstream_cancel_hook_for_testing_;
  std::map<std::string, StreamState> streams_;
  std::map<std::string, PendingCall> pending_calls_;
  base::TimeTicks last_catalog_refresh_time_;
  std::vector<std::string> last_catalog_refresh_provider_ids_;
  base::WeakPtrFactory<NexusLlmHost> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_NEXUS_LLM_HOST_H_
