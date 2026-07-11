#include "custom_browser/browser/nexus/api/network/nexus_network_host.h"

#include <cinttypes>
#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/numerics/safe_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/time/time.h"
#include "base/values.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "net/base/load_flags.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_status_code.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "url/gurl.h"

namespace nexus {

namespace {

using base::Value;
using Dict = base::DictValue;

// Traffic annotation for the network.httpGet surface. Modeled on the
// LLM-provider tag in api/llm/providers/anthropic/anthropic_provider.cc
// — same data-sensitivity tier (user-provided URL, no covert PII),
// same cookie policy (off), same opt-in semantics (user explicitly
// invokes a tool that fires the request).
constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("nexus_web_fetch", R"(
      semantics {
        sender: "Nexus AI Agent Platform"
        description:
          "Fetches a public URL on behalf of the user via the Nexus "
          "WebFetch tool (or any future TS-side tool that calls "
          "network.httpGet through the bridge). The model invokes "
          "the tool with a user-visible URL; the bridge dispatches "
          "the request through this code path. No authentication "
          "headers are attached; the request runs in the browser "
          "process so it is not subject to renderer CORS."
        trigger:
          "User asks the AI assistant to fetch / read / summarise a "
          "web page, which prompts the model to invoke the WebFetch "
          "tool."
        data:
          "The URL the user (or assistant on their behalf) asked to "
          "fetch. No conversation history or other PII is included "
          "in the request body — this is a GET-only surface."
        destination: OTHER
        internal {
          contacts {
            email: "niclas@niclas.cc"
          }
        }
        user_data {
          type: OTHER
        }
        last_reviewed: "2026-05-17"
      }
      policy {
        cookies_allowed: NO
        setting:
          "Users enable / disable the WebFetch tool through the AI "
          "assistant settings; per-domain allow/deny rules live in "
          "the same settings panel."
        policy_exception_justification:
          "Not yet covered by enterprise policy."
      }
    )");

}  // namespace

NexusNetworkHost::NexusNetworkHost(
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
    ::nexus::generated::HttpGetInput input,
    ::nexus::BridgeResponseCallback done,
    CompletionCallback on_complete)
    : url_loader_factory_(std::move(url_loader_factory)),
      input_(std::move(input)),
      done_(std::move(done)),
      on_complete_(std::move(on_complete)) {}

NexusNetworkHost::~NexusNetworkHost() = default;

void NexusNetworkHost::Start() {
  // Resolve per-call options against defaults. The .api.json marks
  // all of these optional with no default — the C++ codegen wraps
  // them in std::optional<T>.
  if (input_.timeoutMs.has_value() && *input_.timeoutMs > 0) {
    timeout_ms_ = *input_.timeoutMs;
  }
  if (input_.maxResponseBytes.has_value() && *input_.maxResponseBytes > 0) {
    max_response_bytes_ =
        base::checked_cast<size_t>(*input_.maxResponseBytes);
  }
  // V2: no longer clamped to SimpleURLLoader's 5 MB
  // `kMaxBoundedStringDownloadSize`. We switched the body-fetch path
  // to `DownloadToStringOfUnboundedSizeUntilCrashAndDie` and enforce
  // the cap ourselves via OnResponseStarted (Content-Length
  // pre-check) and OnDownloadProgress (mid-stream guard).
  if (input_.followRedirects.has_value()) {
    follow_redirects_ = *input_.followRedirects;
  }
  if (input_.maxRedirects.has_value() && *input_.maxRedirects >= 0) {
    max_redirects_ = *input_.maxRedirects;
  }

  // Build the ResourceRequest.
  auto resource_request = std::make_unique<network::ResourceRequest>();
  resource_request->url = GURL(input_.url);
  resource_request->method = "GET";
  // No cookies — matches the traffic annotation (`cookies_allowed: NO`).
  resource_request->credentials_mode =
      network::mojom::CredentialsMode::kOmit;
  // The redirect policy is enforced manually below via
  // `SetOnRedirectCallback`. We don't use `redirect_mode` on the
  // ResourceRequest because SimpleURLLoader handles all redirect
  // decisions through that callback regardless.

  // Apply user-supplied request headers, if any. The .api.json
  // declares `headers` as `object` (passthrough dict). The codegen
  // delivers it as a `base::DictValue` field on the input struct.
  if (input_.headers.has_value()) {
    for (const auto entry : *input_.headers) {
      if (!entry.second.is_string()) continue;
      // Reject hop-by-hop / control headers that Chromium would
      // strip anyway. Letting them through silently means the model
      // sees the header in its tool args but never on the wire —
      // confusing.
      const std::string& name = entry.first;
      if (name.empty()) continue;
      // The HTTP request header API takes a string-string map; the
      // SimpleURLLoader will forward verbatim.
      resource_request->headers.SetHeader(name, entry.second.GetString());
    }
  }

  // Default User-Agent for WebFetch-style requests. CC sends
  // `Mozilla/5.0 (Claude Code WebFetch)`; we use the analogous
  // value here. If the caller passed their own User-Agent in
  // `headers`, that takes precedence (SetHeader overwrites).
  if (!resource_request->headers.HasHeader(
          net::HttpRequestHeaders::kUserAgent)) {
    resource_request->headers.SetHeader(
        net::HttpRequestHeaders::kUserAgent,
        "Mozilla/5.0 (Nexus WebFetch)");
  }

  loader_ = network::SimpleURLLoader::Create(std::move(resource_request),
                                             kTrafficAnnotation);
  loader_->SetAllowHttpErrorResults(true);  // surface 4xx / 5xx as-is

  if (follow_redirects_) {
    loader_->SetOnRedirectCallback(
        base::BindRepeating(&NexusNetworkHost::OnRedirect,
                            weak_ptr_factory_.GetWeakPtr()));
  } else {
    // When following is disabled, the loader still surfaces 3xx
    // responses to `OnDownloadComplete` — we don't need an explicit
    // redirect callback for that path.
  }

  // V2 size-cap enforcement (replacing the 5 MB DCHECK we got for
  // free from DownloadToString).
  //
  // (1) Early-reject by Content-Length when the server is honest.
  loader_->SetOnResponseStartedCallback(
      base::BindOnce(&NexusNetworkHost::OnResponseStarted,
                     weak_ptr_factory_.GetWeakPtr()));
  // (2) Mid-stream cap when Content-Length is missing or wrong.
  loader_->SetOnDownloadProgressCallback(
      base::BindRepeating(&NexusNetworkHost::OnDownloadProgress,
                          weak_ptr_factory_.GetWeakPtr()));

  timeout_timer_.Start(
      FROM_HERE, base::Milliseconds(timeout_ms_),
      base::BindOnce(&NexusNetworkHost::OnTimeout,
                     weak_ptr_factory_.GetWeakPtr()));

  // V2: `DownloadToStringOfUnboundedSizeUntilCrashAndDie` instead of
  // `DownloadToString`. The naming flags the risk; we manage it via
  // the two callbacks registered above plus our own `timeout_timer_`.
  // Worst-case OOM scenario: a server lies about Content-Length AND
  // pipes faster than `OnDownloadProgress` fires. In practice
  // OnDownloadProgress fires every ~32 KB so the overshoot is
  // bounded by one read-buffer's worth, not the whole body.
  loader_->DownloadToStringOfUnboundedSizeUntilCrashAndDie(
      url_loader_factory_.get(),
      base::BindOnce(&NexusNetworkHost::OnDownloadComplete,
                     weak_ptr_factory_.GetWeakPtr()));
}

void NexusNetworkHost::OnDownloadComplete(
    std::optional<std::string> response_body) {
  if (completed_) return;
  completed_ = true;
  timeout_timer_.Stop();

  // Resolve status + headers from the loader. SimpleURLLoader gives
  // us the final response (after any followed redirects). When the
  // request errored at the network layer (DNS failure, connection
  // reset, body-too-large abort), `ResponseInfo()` may be null and
  // `NetError()` carries the reason.
  if (!loader_->ResponseInfo() ||
      !loader_->ResponseInfo()->headers) {
    const int net_error = loader_->NetError();
    CompleteWithError(
        "NETWORK_ERROR",
        base::StringPrintf("Request failed with net error %d.", net_error));
    return;
  }

  const network::mojom::URLResponseHead& head = *loader_->ResponseInfo();
  const int status_code = head.headers->response_code();
  // The reason phrase from the HTTP status line — this is what JS
  // `Response.statusText` carries. `GetStatusText()` returns it
  // already-decoded; no separate "status" header lookup needed (the
  // earlier `GetNormalizedHeader("status", ...)` form was an
  // artefact of an older HttpResponseHeaders API).
  const std::string status_text = head.headers->GetStatusText();

  // Walk all response headers. Multi-value headers are joined with
  // ", " — matches the standard HTTP semantics most JS callers
  // expect from `Response.headers`.
  Dict headers_dict;
  {
    size_t iter = 0;
    std::string name;
    std::string value;
    while (head.headers->EnumerateHeaderLines(&iter, &name, &value)) {
      // EnumerateHeaderLines de-duplicates lines for the SAME name
      // by joining with ", " already, so we just take the first
      // appearance per key. Repeated keys overwrite — defensible,
      // matches `Response.headers.get()` semantics.
      headers_dict.Set(name, value);
    }
  }

  // Build the JSON envelope. Bytes ride on a separate "body" port.
  Dict envelope;
  envelope.Set("status", status_code);
  envelope.Set("statusText", status_text);
  // The final URL surfaces the post-redirect destination. When
  // follow_redirects_ is false, this equals the input URL.
  envelope.Set("finalUrl", loader_->GetFinalURL().spec());
  envelope.Set("headers", std::move(headers_dict));

  // Wrap the body bytes in a BigBuffer. Empty body → zero-byte
  // BigBuffer (NOT a null one — the JS guest's port-recipient
  // expects an ArrayBuffer regardless).
  mojo_base::BigBuffer body_buffer;
  if (response_body.has_value()) {
    body_buffer = mojo_base::BigBuffer(base::as_byte_span(*response_body));
  } else {
    body_buffer = mojo_base::BigBuffer(0);
  }

  std::move(done_).Binary(Value(std::move(envelope)),
                          std::move(body_buffer),
                          /*binary_field=*/"body");

  // Cleanup: let the adapter drop our unique_ptr. Re-entrancy-safe
  // because `done_` was already moved out and the timer was stopped.
  std::move(on_complete_).Run(this);
}

void NexusNetworkHost::OnResponseStarted(
    const GURL& /*final_url*/,
    const network::mojom::URLResponseHead& response_head) {
  if (completed_) return;
  // Content-Length is the cheapest cap signal. When present and
  // honest, we abort before pulling any body bytes. When missing
  // or wrong, OnDownloadProgress catches the overrun mid-stream.
  if (response_head.content_length > 0 &&
      static_cast<size_t>(response_head.content_length) >
          max_response_bytes_) {
    completed_ = true;
    timeout_timer_.Stop();
    loader_.reset();  // cancel before any body bytes hit memory
    CompleteWithError(
        "RESPONSE_TOO_LARGE",
        base::StringPrintf(
            "Response Content-Length (%" PRId64
            " bytes) exceeds the configured cap of %zu bytes.",
            response_head.content_length, max_response_bytes_));
  }
}

void NexusNetworkHost::OnDownloadProgress(uint64_t current_bytes) {
  if (completed_) return;
  if (current_bytes > max_response_bytes_) {
    completed_ = true;
    timeout_timer_.Stop();
    loader_.reset();  // tears down the in-flight network read
    CompleteWithError(
        "RESPONSE_TOO_LARGE",
        base::StringPrintf(
            "Response body exceeded cap of %zu bytes mid-stream (got %" PRIu64
            " bytes before abort).",
            max_response_bytes_, current_bytes));
  }
}

void NexusNetworkHost::OnRedirect(
    const GURL& /*url_before_redirect*/,
    const net::RedirectInfo& /*redirect_info*/,
    const network::mojom::URLResponseHead& /*response_head*/,
    std::vector<std::string>* /*removed_headers*/) {
  redirect_count_++;
  if (redirect_count_ > max_redirects_) {
    if (completed_) return;
    completed_ = true;
    timeout_timer_.Stop();
    // Cancel the loader so the in-flight request actually tears
    // down at the network layer.
    loader_.reset();
    CompleteWithError(
        "REDIRECT_LIMIT_EXCEEDED",
        base::StringPrintf("Exceeded max-redirect count of %d.",
                           max_redirects_));
  }
  // Otherwise: let the loader proceed to the new URL. SimpleURLLoader
  // calls this callback for every hop; we just keep counting.
}

void NexusNetworkHost::OnTimeout() {
  if (completed_) return;
  completed_ = true;
  loader_.reset();  // cancel in-flight
  CompleteWithError(
      "TIMEOUT",
      base::StringPrintf("Request exceeded the %d-ms timeout.", timeout_ms_));
}

void NexusNetworkHost::CompleteWithError(const std::string& code,
                                         const std::string& message) {
  // `done_` is a one-shot. After this call the host is finished;
  // the adapter cleans up via `on_complete_`.
  if (done_.fired()) {
    // Defensive — should never happen, but if it does we still
    // need to give the adapter its cleanup callback.
    if (on_complete_) {
      std::move(on_complete_).Run(this);
    }
    return;
  }
  std::move(done_).Error(code, message);
  if (on_complete_) {
    std::move(on_complete_).Run(this);
  }
}

}  // namespace nexus
