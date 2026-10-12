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
#include "custom_browser/browser/nexus/api/network/nexus_guest_request.h"
#include "custom_browser/browser/nexus/bridge_response_callback.h"
#include "services/network/public/cpp/simple_url_loader.h"

namespace network {
class SharedURLLoaderFactory;
}  // namespace network

namespace nexus {

// Request headers a `network.*` caller may not set (hop-by-hop, framing,
// Host, Cookie, Origin, `sec-*`, `proxy-*`; case-insensitive). httpGet /
// httpRequest drop them; streamOpen refuses them.
bool IsReservedNetworkRequestHeader(const std::string& name);

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
  // Egress policy opt-out (`allowPrivateNetwork`). When false, the IP the
  // socket connects to must be publicly routable (`network_egress_policy.h`),
  // at the first response and at every followed redirect (skipped through a
  // proxy, whose address is the socket's peer). The default is per route:
  // false for `httpRequest`, true for `httpGet` (its pre-policy reach).
  bool allow_private_network = false;
  // Headers a `{{secret:NAME}}` was substituted into, and the origin those
  // secrets are bound to (serialized). A followed redirect that leaves the
  // origin goes ahead without these headers, the way Fetch drops
  // Authorization on a cross-origin redirect.
  std::vector<std::string> secret_headers;
  std::string secret_origin;
  // What `finalUrl` reports: the guest's `{{secret:NAME}}` /
  // `{{oauth:NAME}}` placeholders in place of the query values they expanded
  // to. Empty (the final URL as-is) when nothing was expanded.
  GuestVisibleUrl visible_url;

  // The GET surface as the spec: every GET field mapped, no method / body.
  static NexusHttpRequestSpec FromHttpGet(::nexus::generated::HttpGetInput in);
  // The general surface as the spec. `method` is upper-cased here; the
  // adapter validated the token and the body / method pairing.
  static NexusHttpRequestSpec FromHttpRequest(
      ::nexus::generated::HttpRequestInput in);
};

// Per-request worker wrapping one `network::SimpleURLLoader` for a
// `network.httpGet` / `network.httpRequest` call. Fires `done` once, then
// `OnComplete` so the owning adapter drops it. UI thread only.
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
  // Defaults for unset optional inputs. The unbounded-download API is capped
  // three ways: kDefaultMaxResponseBytes, an early Content-Length reject, and a
  // mid-stream abort on accumulated bytes.
  static constexpr int kDefaultTimeoutMs = 60'000;
  static constexpr size_t kDefaultMaxResponseBytes = 50 * 1024 * 1024;
  static constexpr int kDefaultMaxRedirects = 10;

  // Download completion: builds the envelope, fires `done_`, then
  // `on_complete_`. `response_body` is nullopt on a network-layer error; 4xx/5xx
  // bodies still flow (SetAllowHttpErrorResults).
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

  // Per-3xx hop when `follow_redirects_`: enforces the hop budget and applies
  // the egress policy to the answering endpoint and to an IP-literal target.
  // Unregistered otherwise; the 3xx then reaches `OnDownloadComplete`.
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
