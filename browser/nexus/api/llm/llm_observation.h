#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_OBSERVATION_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_OBSERVATION_H_

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/values.h"

namespace net {
class HttpResponseHeaders;
}  // namespace net

namespace nexus {

// Host-captured wire evidence for ONE provider request — the part of the
// exchange that the normalized response necessarily throws away: HTTP response
// headers (never read before this), the raw body `id` / `service_tier`, and the
// exact size of the body we put on the wire.
//
// WHY IT EXISTS. The guest cannot see any of this. Its own request-byte count
// under-counts (prompt holders and tool refs resolve here, host-side), and the
// response reaches it rebuilt field-by-field through a Mojo struct. Endpoint
// integrity in nexus_web uses these markers to tell whether an API relay is
// really fronting the vendor infrastructure it claims.
//
// COLLECTION IS OPT-IN PER REQUEST. Nothing here is gathered unless the guest
// set `captureObservations` on that request (mojom `capture_observations`).
// With the flag clear, the capture helpers are never called, no headers are
// read, and no observation rides back — so the default state of the product
// collects nothing at all.
//
// PRIVACY. `CaptureResponseHeaders` copies only the allowlist in
// `ObservationHeaderAllowlist()`: vendor telemetry and rate-limit fields.
// Authorization, cookies, and everything else are never touched, and no
// response *content* is ever recorded here.
struct LlmObservation {
  LlmObservation();
  LlmObservation(const LlmObservation&);
  LlmObservation& operator=(const LlmObservation&);
  ~LlmObservation();

  std::optional<int> http_status;
  // Lowercased header name -> value, allowlist only.
  std::map<std::string, std::string> headers;
  std::optional<std::string> response_id;
  std::optional<std::string> service_tier;
  // Model id exactly as the provider body reported it, pre-normalization.
  std::optional<std::string> raw_model_id;
  // SHA-256 (lowercase hex) of the outbound request body.
  std::optional<std::string> request_body_digest;
  std::optional<int> request_body_bytes;
  std::optional<int> ttfb_ms;
};

// The only response headers ever copied into an observation. Lowercase.
// Chosen so the SET and FORMAT carry the signal (a relay proxying to other
// infrastructure must forge the whole correlated telemetry surface), while no
// entry can carry user content or a credential.
const std::vector<std::string>& ObservationHeaderAllowlist();

// Copies status + allowlisted headers off a response head. No-op when
// `headers` is null (net error before a response head arrived).
void CaptureResponseHeaders(const net::HttpResponseHeaders* headers,
                            LlmObservation* observation);

// Records the size and SHA-256 of the body actually sent upstream.
void CaptureRequestBody(std::string_view body, LlmObservation* observation);

// Pulls `id` / `service_tier` / `model` out of a parsed provider response body.
// Tolerant: absent fields are simply not recorded.
void CaptureResponseBodyMarkers(const base::DictValue& body,
                                LlmObservation* observation);

// True when this request opted in. Providers MUST check this before doing any
// capture work — with it false nothing is read, nothing is hashed, and nothing
// is emitted. This is the single gate the guest controls per request.
bool ShouldCaptureObservations(const base::DictValue& call_options);

// Serializes an observation into the `{"type":"observation", …}` stream chunk
// the streaming providers hand to their StreamChunkCallback. The host parses
// it back out and parks it on the stream; it is never a model event and never
// reaches the transcript.
base::DictValue BuildObservationChunk(const LlmObservation& observation);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_LLM_OBSERVATION_H_
