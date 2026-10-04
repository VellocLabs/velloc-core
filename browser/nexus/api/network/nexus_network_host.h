#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_NETWORK_NEXUS_NETWORK_HOST_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_NETWORK_NEXUS_NETWORK_HOST_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "custom_browser/browser/nexus/api/network/network_api.gen.h"
#include "custom_browser/browser/nexus/bridge_response_callback.h"
#include "services/network/public/cpp/simple_url_loader.h"

namespace network {
class SharedURLLoaderFactory;
}  // namespace network

namespace nexus {

// One HTTP request as the host runs it: the common form of
// `network.httpGet` (GET, no body) and `network.httpRequest` (any method,
// an optional body). Built by the adapter from the generated inputs.
struct NexusHttpRequestSpec {
  NexusHttpRequestSpec();
  NexusHttpRequestSpec(NexusHttpRequestSpec&&);
  NexusHttpRequestSpec& operator=(NexusHttpRequestSpec&&);
  ~NexusHttpRequestSpec();

  std::string url;
  // Upper-cased HTTP method token. GET by default.
  std::string method = "GET";
  std::optional<base::DictValue> headers;
  // Request body; absent for GET / HEAD and when the caller sent none.
  std::optional<std::vector<uint8_t>> body;
  std::optional<int32_t> timeout_ms;
  std::optional<int32_t> max_response_bytes;
  std::optional<bool> follow_redirects;
  std::optional<int32_t> max_redirects;
  // Egress policy opt-out (`allowPrivateNetwork`): a request the person,
  // not a plugin or the model, aimed at a local server. Default off: the
  // IP the socket connects to must be publicly routable
  // (`network_egress_policy.h`), at the first response and at every
  // followed redirect.
  bool allow_private_network = false;

  // The GET surface as the spec: every GET field mapped, no method / body.
  static NexusHttpRequestSpec FromHttpGet(::nexus::generated::HttpGetInput in);
  // The general surface as the spec. `method` is upper-cased here; the
  // adapter validated the token and the body / method pairing.
  static NexusHttpRequestSpec FromHttpRequest(
      ::nexus::generated::HttpRequestInput in);
};

// Per-request worker that wraps a single `network::SimpleURLLoader`
// to fulfil one `network.httpGet` / `network.httpRequest` bridge call.
//
// Lifetime: owned by `NexusNetworkBridgeAdapter::in_flight_`. The
// host fires `done` (its captured `BridgeResponseCallback`) once
// the request completes (success / failure / timeout / abort /
// egress refusal), then invokes the `OnComplete` cleanup callback so
// the adapter can drop its `unique_ptr` (which destroys the loader
// and cancels any in-flight network work).
//
// All members live on the UI thread — `SimpleURLLoader` and
// `SharedURLLoaderFactory` are UI-thread objects, and the adapter's
// `Handle*OnUI` are the only entry points.
class NexusNetworkHost {
 public:
  using CompletionCallback = base::OnceCallback<void(NexusNetworkHost*)>;

  NexusNetworkHost(
      scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
      NexusHttpRequestSpec spec,
      ::nexus::BridgeResponseCallback done,
      CompletionCallback on_complete);
  ~NexusNetworkHost();

  NexusNetworkHost(const NexusNetworkHost&) = delete;
  NexusNetworkHost& operator=(const NexusNetworkHost&) = delete;

  // Kick off the request. Constructs the `network::ResourceRequest`
  // with the configured method / headers / body / redirect mode, hands
  // it to a fresh `SimpleURLLoader`, and starts the download. Safe to
  // call once.
  void Start();

 private:
  // Defaults — applied when the corresponding optional input is unset.
  //
  // V2: lifted from 5 MB to 50 MB to support large-binary persistence
  // (PDFs, archives, large API exports) and the new text-overflow
  // disk-persistence flow in `WebFetchTool/utils.ts`. The 5 MB
  // ceiling came from `SimpleURLLoader::DownloadToString`'s built-in
  // `DCHECK_LE(max_body_size, kMaxBoundedStringDownloadSize)`. To
  // exceed that, we now use
  // `DownloadToStringOfUnboundedSizeUntilCrashAndDie` (the API's
  // intentionally-alarming name signals: no built-in cap, caller is
  // responsible for bounding memory). We bound it three ways:
  //   (1) `kDefaultMaxResponseBytes` ceiling (default 50 MB).
  //   (2) `SetOnResponseStartedCallback` — early-reject if
  //       `Content-Length` exceeds the ceiling (cheapest path; no
  //       body bytes pulled).
  //   (3) `SetOnDownloadProgressCallback` — abort mid-stream if
  //       accumulated bytes exceed the ceiling (catches servers
  //       that lie about Content-Length or omit it).
  static constexpr int kDefaultTimeoutMs = 60'000;
  static constexpr size_t kDefaultMaxResponseBytes = 50 * 1024 * 1024;
  static constexpr int kDefaultMaxRedirects = 10;

  // Called by `SimpleURLLoader::DownloadToStringOfUnboundedSizeUntilCrashAndDie`
  // when the response is complete (success or HTTP-error or
  // network-failure). Builds the success envelope, fires `done_`,
  // then invokes `on_complete_` so the owning adapter drops the
  // unique_ptr.
  //
  // `response_body` is `std::nullopt` when the request errored at
  // the network layer (DNS / timeout / cap-exceeded / cancelled).
  // For 4xx/5xx HTTP responses we set `SetAllowHttpErrorResults(true)`
  // so the body still flows through.
  void OnDownloadComplete(std::optional<std::string> response_body);

  // Fires once when response headers are received. Two checks:
  //   - the egress policy on `response_head.remote_endpoint`, the IP
  //     the socket actually connected to (the only check that holds
  //     against DNS rebinding — the guest never sees an IP);
  //   - early-abort downloads whose advertised `Content-Length`
  //     would exceed `max_response_bytes_`.
  void OnResponseStarted(const GURL& final_url,
                         const network::mojom::URLResponseHead& response_head);

  // Fires repeatedly as body bytes accumulate. We use it to abort
  // mid-stream if the actual body size exceeds `max_response_bytes_`.
  // Belt-and-suspenders to `OnResponseStarted`: catches servers
  // whose Content-Length is missing or wrong.
  void OnDownloadProgress(uint64_t current_bytes);

  // Called by `SimpleURLLoader::SetOnRedirectCallback` for each
  // 3xx redirect when `follow_redirects_` is true. Tracks hop count;
  // cancels with an error when the hop budget is exceeded. Applies
  // the egress policy twice per hop: to the endpoint that answered
  // the 3xx (`response_head.remote_endpoint`) and, when the new
  // location's host is an IP literal, to that literal before the
  // next dial (a hostname is judged when it answers).
  //
  // When `follow_redirects_` is false, this callback is NOT
  // registered — the loader instead delivers the 3xx response to
  // `OnDownloadComplete` with the body being whatever the upstream
  // sent (usually empty). The caller uses `final_url_` /
  // `response_headers_` to find the Location header.
  void OnRedirect(const GURL& url_before_redirect,
                  const net::RedirectInfo& redirect_info,
                  const network::mojom::URLResponseHead& response_head,
                  std::vector<std::string>* removed_headers);

  // Hard-timeout fires this. Cancels the loader and completes with
  // a TIMEOUT error envelope.
  void OnTimeout();

  // The egress refusal for the endpoint a response came from, or
  // nullopt when it is allowed (or the policy is opted out).
  std::optional<std::string> EgressRefusalForEndpoint(
      const network::mojom::URLResponseHead& response_head) const;

  // Cancel the loader and fire `EGRESS_BLOCKED` with `reason`.
  void AbortForEgress(const std::string& reason);

  // Fire `done_` with a typed error and unblock the adapter cleanup.
  void CompleteWithError(const std::string& code, const std::string& message);

  // Acquired once at construction.
  scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory_;
  NexusHttpRequestSpec spec_;
  ::nexus::BridgeResponseCallback done_;
  CompletionCallback on_complete_;

  // Resolved defaults applied at Start().
  int timeout_ms_ = kDefaultTimeoutMs;
  size_t max_response_bytes_ = kDefaultMaxResponseBytes;
  bool follow_redirects_ = false;
  int max_redirects_ = kDefaultMaxRedirects;

  // Per-request mutable state.
  std::unique_ptr<network::SimpleURLLoader> loader_;
  base::OneShotTimer timeout_timer_;
  int redirect_count_ = 0;
  bool completed_ = false;

  base::WeakPtrFactory<NexusNetworkHost> weak_ptr_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_NETWORK_NEXUS_NETWORK_HOST_H_
