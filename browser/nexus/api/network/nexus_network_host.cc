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
#include "base/strings/string_util.h"
#include "base/values.h"
#include "custom_browser/browser/nexus/api/network/network_egress_policy.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "net/base/load_flags.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "net/base/ip_endpoint.h"
#include "net/http/http_status_code.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "url/gurl.h"
#include "url/origin.h"

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
          "headers are attached by the bridge; the request runs in "
          "the browser process so it is not subject to renderer "
          "CORS, and only publicly routable endpoints are dialed "
          "(network_egress_policy.h) unless the person opted out."
        trigger:
          "User asks the AI assistant to fetch / read / summarise a "
          "web page, which prompts the model to invoke the WebFetch "
          "tool."
        data:
          "The URL the user (or assistant on their behalf) asked to "
          "fetch, and for network.httpRequest the request body the "
          "calling tool or plugin composed. No conversation history "
          "is attached by the bridge itself."
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

// Request headers the caller may not set: the loader owns the framing
// and connection headers, and the cookie jar is off for this surface
// (`cookies_allowed: NO`). Compared case-insensitively.
bool IsReservedNetworkRequestHeader(const std::string& name) {
  static constexpr const char* kReserved[] = {
      "host",       "content-length",    "connection",      "keep-alive",
      "transfer-encoding", "upgrade",   "te",              "trailer",
      "proxy-connection",  "proxy-authorization",            "cookie",
      "cookie2",    "origin",            "via",             "expect",
  };
  for (const char* reserved : kReserved) {
    if (base::EqualsCaseInsensitiveASCII(name, reserved)) {
      return true;
    }
  }
  // `sec-*` and `proxy-*` are the browser's own.
  return base::StartsWith(name, "sec-", base::CompareCase::INSENSITIVE_ASCII) ||
         base::StartsWith(name, "proxy-", base::CompareCase::INSENSITIVE_ASCII);
}

NexusHttpRequestSpec::NexusHttpRequestSpec() = default;
NexusHttpRequestSpec::NexusHttpRequestSpec(NexusHttpRequestSpec&&) = default;
NexusHttpRequestSpec& NexusHttpRequestSpec::operator=(NexusHttpRequestSpec&&) =
    default;
NexusHttpRequestSpec::~NexusHttpRequestSpec() = default;

// static
NexusHttpRequestSpec NexusHttpRequestSpec::FromHttpGet(
    ::nexus::generated::HttpGetInput in) {
  NexusHttpRequestSpec spec;
  spec.url = std::move(in.url);
  spec.method = "GET";
  spec.headers = std::move(in.headers);
  spec.timeout_ms = in.timeoutMs;
  spec.max_response_bytes = in.maxResponseBytes;
  spec.follow_redirects = in.followRedirects;
  spec.max_redirects = in.maxRedirects;
  // Opted out unless the caller asks for the policy (see the adapter).
  spec.allow_private_network = in.allowPrivateNetwork.value_or(true);
  return spec;
}

// static
NexusHttpRequestSpec NexusHttpRequestSpec::FromHttpRequest(
    ::nexus::generated::HttpRequestInput in) {
  NexusHttpRequestSpec spec;
  spec.url = std::move(in.url);
  spec.method = base::ToUpperASCII(in.method.value_or("GET"));
  spec.headers = std::move(in.headers);
  if (!in.body.empty()) {  // generated as a plain vector, empty when absent
    spec.body = std::move(in.body);
  }
  spec.timeout_ms = in.timeoutMs;
  spec.max_response_bytes = in.maxResponseBytes;
  spec.follow_redirects = in.followRedirects;
  spec.max_redirects = in.maxRedirects;
  spec.allow_private_network = in.allowPrivateNetwork.value_or(false);
  return spec;
}

NexusNetworkHost::NexusNetworkHost(
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
    NexusHttpRequestSpec spec,
    ::nexus::BridgeResponseCallback done,
    CompletionCallback on_complete)
    : url_loader_factory_(std::move(url_loader_factory)),
      spec_(std::move(spec)),
      done_(std::move(done)),
      on_complete_(std::move(on_complete)) {}

NexusNetworkHost::~NexusNetworkHost() = default;

void NexusNetworkHost::Start() {
  // Resolve per-call options against defaults. The .api.json marks
  // all of these optional with no default — the C++ codegen wraps
  // them in std::optional<T>.
  if (spec_.timeout_ms.has_value() && *spec_.timeout_ms > 0) {
    timeout_ms_ = *spec_.timeout_ms;
  }
  if (spec_.max_response_bytes.has_value() && *spec_.max_response_bytes > 0) {
    max_response_bytes_ =
        base::checked_cast<size_t>(*spec_.max_response_bytes);
  }
  // V2: no longer clamped to SimpleURLLoader's 5 MB
  // `kMaxBoundedStringDownloadSize`. We switched the body-fetch path
  // to `DownloadToStringOfUnboundedSizeUntilCrashAndDie` and enforce
  // the cap ourselves via OnResponseStarted (Content-Length
  // pre-check) and OnDownloadProgress (mid-stream guard).
  if (spec_.follow_redirects.has_value()) {
    follow_redirects_ = *spec_.follow_redirects;
  }
  if (spec_.max_redirects.has_value() && *spec_.max_redirects >= 0) {
    max_redirects_ = *spec_.max_redirects;
  }

  // Build the ResourceRequest.
  auto resource_request = std::make_unique<network::ResourceRequest>();
  resource_request->url = GURL(spec_.url);
  resource_request->method = spec_.method;
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
  if (spec_.headers.has_value()) {
    for (const auto entry : *spec_.headers) {
      if (!entry.second.is_string()) continue;
      const std::string& name = entry.first;
      if (name.empty()) continue;
      // Drop the headers the loader owns or Chromium would strip
      // anyway (hop-by-hop, framing, the cookie jar). Letting them
      // through silently means the caller sees the header in its
      // arguments but never on the wire — confusing; and a caller
      // must not smuggle a Host or a Cookie through this surface.
      if (IsReservedNetworkRequestHeader(name)) continue;
      // The adapter refused these already; skipping here too keeps a new
      // caller from reaching SetHeader's CHECK (a browser crash).
      if (InvalidGuestHeader(name, entry.second.GetString())) continue;
      // The HTTP request header API takes a string-string map; the
      // SimpleURLLoader will forward verbatim.
      resource_request->headers.SetHeader(name, entry.second.GetString());
    }
  }
  // A body's Content-Type: the caller's, else octet-stream.
  const bool has_body = spec_.body.has_value() && !spec_.body->empty();
  std::string content_type = "application/octet-stream";
  if (has_body) {
    std::optional<std::string> given =
        resource_request->headers.GetHeader(net::HttpRequestHeaders::kContentType);
    if (given.has_value() && !given->empty()) {
      content_type = *given;
    }
    // The loader sets Content-Type / Content-Length itself from the
    // attached upload; a copy in the header map would duplicate it.
    resource_request->headers.RemoveHeader(net::HttpRequestHeaders::kContentType);
  }

  // Default User-Agent for WebFetch-style requests. CC sends
  // `Mozilla/5.0 (Claude Code WebFetch)`; we use the analogous
  // value here. If the caller passed their own User-Agent in
  // `headers`, that takes precedence (SetHeader overwrites).
  if (!resource_request->headers.HasHeader(
          net::HttpRequestHeaders::kUserAgent)) {
    resource_request->headers.SetHeader(
        net::HttpRequestHeaders::kUserAgent,
        "Mozilla/5.0 (Velloc WebFetch)");
  }

  loader_ = network::SimpleURLLoader::Create(std::move(resource_request),
                                             kTrafficAnnotation);
  loader_->SetAllowHttpErrorResults(true);  // surface 4xx / 5xx as-is
  // Pre-send egress enforcement (`network_egress_policy.h`): the network
  // service refuses a connection whose peer is loopback or private before
  // the request is written, so a hostname that resolves to a LAN device
  // never receives it. Covers every redirect hop too. The response-time
  // checks below stay for the ranges Chromium counts as public
  // (carrier-grade NAT, multicast).
  if (!spec_.allow_private_network) {
    loader_->SetURLLoaderFactoryOptions(
        network::mojom::kURLLoadOptionBlockLocalRequest);
  }
  if (has_body) {
    loader_->AttachStringForUpload(
        std::string(spec_.body->begin(), spec_.body->end()), content_type);
  }

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
    if (!spec_.allow_private_network) {
      if (std::optional<std::string> refusal =
              EgressRefusalForNetError(net_error, GURL(spec_.url))) {
        completed_ = false;  // AbortForEgress owns completion.
        AbortForEgress(*refusal);
        return;
      }
    }
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
  // follow_redirects_ is false, this equals the input URL. A query value a
  // secret was expanded into shows the guest's placeholder, never the secret.
  envelope.Set("finalUrl", spec_.visible_url.For(loader_->GetFinalURL()));
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
  // The egress policy on the IP the socket connected to. Headers are
  // in, no body byte has been read: aborting here leaks nothing of
  // the body to the caller.
  if (std::optional<std::string> refusal = EgressRefusalForEndpoint(response_head)) {
    AbortForEgress(*refusal);
    return;
  }
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
    const net::RedirectInfo& redirect_info,
    const network::mojom::URLResponseHead& response_head,
    std::vector<std::string>* removed_headers) {
  if (completed_) return;
  // The hop that answered the 3xx was dialed: judge its endpoint. Then
  // the next location, when its host is a literal, before the dial; a
  // hostname is judged when it answers (OnRedirect again, or
  // OnResponseStarted at the end).
  if (std::optional<std::string> refusal = EgressRefusalForEndpoint(response_head)) {
    AbortForEgress(*refusal);
    return;
  }
  if (!spec_.allow_private_network) {
    if (std::optional<std::string> refusal =
            EgressRefusalForLiteralHost(redirect_info.new_url)) {
      AbortForEgress(base::StringPrintf("redirect target refused: %s",
                                        refusal->c_str()));
      return;
    }
  }
  if (!spec_.secret_headers.empty() &&
      url::Origin::Create(redirect_info.new_url).Serialize() !=
          spec_.secret_origin) {
    removed_headers->insert(removed_headers->end(),
                            spec_.secret_headers.begin(),
                            spec_.secret_headers.end());
  }
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

std::optional<std::string> NexusNetworkHost::EgressRefusalForEndpoint(
    const network::mojom::URLResponseHead& response_head) const {
  if (spec_.allow_private_network) {
    return std::nullopt;
  }
  // Through a proxy the socket's peer is the proxy (often a local one,
  // 127.0.0.1:7890), and the proxy resolves the origin's name: there is
  // no origin IP to judge. The pre-dial literal-host check still runs.
  if (!response_head.proxy_chain.is_direct()) {
    return std::nullopt;
  }
  // `remote_endpoint` is the peer the socket connected to; invalid
  // (no address) for a response served without a dial (the cache, a
  // service worker — neither applies to this loader), which the policy
  // lets through: nothing was reached.
  const net::IPAddress& address = response_head.remote_endpoint.address();
  std::optional<std::string> refusal = EgressRefusalFor(address);
  if (!refusal) {
    return std::nullopt;
  }
  return base::StringPrintf("%s resolved to %s, a %s",
                            std::string(GURL(spec_.url).host()).c_str(),
                            address.ToString().c_str(), refusal->c_str());
}

void NexusNetworkHost::AbortForEgress(const std::string& reason) {
  if (completed_) return;
  completed_ = true;
  timeout_timer_.Stop();
  loader_.reset();  // tears down the connection before any body read
  CompleteWithError(
      "EGRESS_BLOCKED",
      base::StringPrintf(
          "Refused by the egress policy: %s. Only publicly routable "
          "hosts are reachable through network.* (allowPrivateNetwork "
          "opts a person's own request out).",
          reason.c_str()));
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
