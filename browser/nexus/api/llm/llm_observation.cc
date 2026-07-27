#include "custom_browser/browser/nexus/api/llm/llm_observation.h"

#include <array>
#include <vector>

#include "base/containers/span.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "crypto/hash.h"
#include "net/http/http_response_headers.h"

namespace nexus {

LlmObservation::LlmObservation() = default;
LlmObservation::LlmObservation(const LlmObservation&) = default;
LlmObservation& LlmObservation::operator=(const LlmObservation&) = default;
LlmObservation::~LlmObservation() = default;

const std::vector<std::string>& ObservationHeaderAllowlist() {
  // Vendor telemetry + rate-limit families only. Deliberately short: every
  // entry is a header we are willing to carry into the guest process, and the
  // detection value is in presence/format, not in reading more fields.
  static const base::NoDestructor<std::vector<std::string>> kAllowlist({
      // Anthropic
      "request-id",
      "anthropic-organization-id",
      "anthropic-ratelimit-requests-remaining",
      "anthropic-ratelimit-tokens-remaining",
      "anthropic-ratelimit-requests-reset",
      // OpenAI
      "x-request-id",
      "openai-organization",
      "openai-processing-ms",
      "openai-version",
      // Generic infrastructure markers (a relay's own fronting shows up here)
      "server",
      "via",
      "cf-ray",
  });
  return *kAllowlist;
}

void CaptureResponseHeaders(const net::HttpResponseHeaders* headers,
                            LlmObservation* observation) {
  if (!headers || !observation) {
    return;
  }
  observation->http_status = headers->response_code();
  for (const std::string& name : ObservationHeaderAllowlist()) {
    if (std::optional<std::string> value = headers->GetNormalizedHeader(name);
        value.has_value()) {
      observation->headers[name] = *value;
    }
  }
}

void CaptureRequestBody(std::string_view body, LlmObservation* observation) {
  if (!observation) {
    return;
  }
  observation->request_body_bytes = static_cast<int>(body.size());
  const std::array<uint8_t, crypto::hash::kSha256Size> digest =
      crypto::hash::Sha256(base::as_byte_span(body));
  observation->request_body_digest =
      base::ToLowerASCII(base::HexEncode(digest));
}

bool ShouldCaptureObservations(const base::DictValue& call_options) {
  return call_options.FindBool("captureObservations").value_or(false);
}

base::DictValue BuildObservationChunk(const LlmObservation& observation) {
  base::DictValue chunk;
  chunk.Set("type", "observation");
  if (observation.http_status.has_value()) {
    chunk.Set("httpStatus", *observation.http_status);
  }
  if (!observation.headers.empty()) {
    base::DictValue headers;
    for (const auto& [name, value] : observation.headers) {
      headers.Set(name, value);
    }
    chunk.Set("headers", std::move(headers));
  }
  if (observation.response_id.has_value()) {
    chunk.Set("responseId", *observation.response_id);
  }
  if (observation.service_tier.has_value()) {
    chunk.Set("serviceTier", *observation.service_tier);
  }
  if (observation.raw_model_id.has_value()) {
    chunk.Set("rawModelId", *observation.raw_model_id);
  }
  if (observation.request_body_digest.has_value()) {
    chunk.Set("requestBodyDigest", *observation.request_body_digest);
  }
  if (observation.request_body_bytes.has_value()) {
    chunk.Set("requestBodyBytes", *observation.request_body_bytes);
  }
  if (observation.ttfb_ms.has_value()) {
    chunk.Set("ttfbMs", *observation.ttfb_ms);
  }
  return chunk;
}

void CaptureResponseBodyMarkers(const base::DictValue& body,
                                LlmObservation* observation) {
  if (!observation) {
    return;
  }
  if (const std::string* id = body.FindString("id")) {
    observation->response_id = *id;
  }
  if (const std::string* service_tier = body.FindString("service_tier")) {
    observation->service_tier = *service_tier;
  }
  if (const std::string* model = body.FindString("model")) {
    observation->raw_model_id = *model;
  }
}

}  // namespace nexus
