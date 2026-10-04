#include "custom_browser/browser/nexus/api/network/nexus_network_bridge_adapter.h"

#include <algorithm>
#include <utility>

#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/storage_partition.h"
#include "custom_browser/browser/nexus/api/network/network_egress_policy.h"
#include "custom_browser/browser/nexus/api/network/nexus_network_host.h"
#include "net/http/http_util.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "url/gurl.h"

namespace nexus {

namespace {

// Hard cap on the input URL length. CC's WebFetch validator uses
// 2000; matching that here gives forward-compatible error messages
// if a user migrates settings between CC and Nexus. The cap is also
// a defense against absurdly long URLs that would otherwise consume
// loader-state memory before any redirect / DNS work happens.
constexpr size_t kMaxUrlLengthChars = 2000;

// Hard cap on a request body for `network.httpRequest`. A plugin's
// `$.http.fetch` body is a string in the declaration; 16 MiB is far
// above any JSON a hook posts and keeps one request from pinning the
// loader's upload buffer on a runaway caller.
constexpr size_t kMaxRequestBodyBytes = 16 * 1024 * 1024;

// Methods that never leave this surface: CONNECT opens a tunnel, TRACE
// and TRACK echo the request (and any header the browser added).
bool IsForbiddenMethod(const std::string& upper) {
  return upper == "CONNECT" || upper == "TRACE" || upper == "TRACK";
}

// Shared pre-flight for both surfaces: the URL as a string, its parse,
// the scheme, no embedded credentials, and — the egress policy's cheap
// half — an IP-literal host in a refused range before any dial. Returns
// false after firing `done` with the refusal.
bool ValidateRequestUrl(const std::string& url,
                        bool allow_private_network,
                        ::nexus::BridgeResponseCallback& done) {
  if (url.empty()) {
    std::move(done).Error("INVALID_URL", "URL must be a non-empty string.");
    return false;
  }
  if (url.size() > kMaxUrlLengthChars) {
    std::move(done).Error(
        "INVALID_URL",
        base::StringPrintf(
            "URL exceeds maximum length of %zu characters (got %zu).",
            kMaxUrlLengthChars, url.size()));
    return false;
  }
  GURL gurl(url);
  if (!gurl.is_valid()) {
    std::move(done).Error(
        "INVALID_URL",
        base::StringPrintf("Could not parse URL: %s", url.c_str()));
    return false;
  }
  if (!gurl.SchemeIsHTTPOrHTTPS()) {
    const std::string scheme(gurl.scheme());
    std::move(done).Error(
        "INVALID_URL",
        base::StringPrintf(
            "Only http and https schemes are supported (got %s).",
            scheme.c_str()));
    return false;
  }
  if (gurl.has_username() || gurl.has_password()) {
    // Defense-in-depth: URLs with embedded credentials should not
    // leak through this surface. The C++ Chromium URLLoader would
    // strip these anyway in most paths, but rejecting up-front
    // makes the contract explicit.
    std::move(done).Error(
        "INVALID_URL",
        "URLs may not embed credentials (user:pass@host) for network.*.");
    return false;
  }
  if (!allow_private_network) {
    if (std::optional<std::string> refusal = EgressRefusalForLiteralHost(gurl)) {
      std::move(done).Error(
          "EGRESS_BLOCKED",
          base::StringPrintf(
              "Refused by the egress policy: %s. Only publicly routable "
              "hosts are reachable through network.*.",
              refusal->c_str()));
      return false;
    }
  }
  return true;
}

}  // namespace

NexusNetworkBridgeAdapter::NexusNetworkBridgeAdapter(
    NexusBridgeResponseDelegate* delegate,
    content::BrowserContext* browser_context)
    : ::nexus::generated::NetworkApiBase(delegate) {
  // Acquire a `SharedURLLoaderFactory` from the profile's default
  // storage partition. This is the SAME factory the LLM providers
  // use (see api/llm/llm_broker_factory.cc:44-45) — i.e. requests
  // here will use the same cookie jar, same proxy config, same
  // network-service routing.
  if (browser_context) {
    url_loader_factory_ = browser_context->GetDefaultStoragePartition()
                              ->GetURLLoaderFactoryForBrowserProcess();
  }
}

NexusNetworkBridgeAdapter::~NexusNetworkBridgeAdapter() = default;

void NexusNetworkBridgeAdapter::HandleHttpGetOnUI(
    ::nexus::generated::HttpGetInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  if (!url_loader_factory_) {
    std::move(done).Error(
        "NETWORK_ERROR",
        "Network capability not initialised: no URLLoaderFactory was "
        "acquired at construction (was the BrowserContext null?).");
    return;
  }
  if (!ValidateRequestUrl(input.url, input.allowPrivateNetwork.value_or(false),
                          done)) {
    return;
  }
  StartRequest(NexusHttpRequestSpec::FromHttpGet(std::move(input)),
               std::move(done));
}

void NexusNetworkBridgeAdapter::HandleHttpRequestOnUI(
    ::nexus::generated::HttpRequestInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  if (!url_loader_factory_) {
    std::move(done).Error(
        "NETWORK_ERROR",
        "Network capability not initialised: no URLLoaderFactory was "
        "acquired at construction (was the BrowserContext null?).");
    return;
  }
  if (!ValidateRequestUrl(input.url, input.allowPrivateNetwork.value_or(false),
                          done)) {
    return;
  }
  // The method: an HTTP token, upper-cased; the tunnelling and echoing
  // methods never.
  const std::string method = base::ToUpperASCII(input.method.value_or("GET"));
  if (method.empty() || !net::HttpUtil::IsValidHeaderName(method) ||
      IsForbiddenMethod(method)) {
    std::move(done).Error(
        "INVALID_METHOD",
        base::StringPrintf("Unsupported HTTP method: %s", method.c_str()));
    return;
  }
  if (input.body.has_value() && !input.body->empty()) {
    if (method == "GET" || method == "HEAD") {
      std::move(done).Error("INVALID_REQUEST",
                            "A GET or HEAD request may not carry a body.");
      return;
    }
    if (input.body->size() > kMaxRequestBodyBytes) {
      std::move(done).Error(
          "REQUEST_TOO_LARGE",
          base::StringPrintf("Request body of %zu bytes exceeds the cap of %zu.",
                             input.body->size(), kMaxRequestBodyBytes));
      return;
    }
  }
  StartRequest(NexusHttpRequestSpec::FromHttpRequest(std::move(input)),
               std::move(done));
}

void NexusNetworkBridgeAdapter::StartRequest(NexusHttpRequestSpec spec,
                                             ::nexus::BridgeResponseCallback done) {
  // Construct a per-request host. The host owns its `SimpleURLLoader`
  // and fires the completion callback when the body lands (or the
  // request fails / times out / is refused). Captures a raw `this` plus
  // the host pointer for the lookup-and-erase in `OnHttpGetComplete`.
  auto host = std::make_unique<NexusNetworkHost>(
      url_loader_factory_, std::move(spec), std::move(done),
      base::BindOnce(&NexusNetworkBridgeAdapter::OnHttpGetComplete,
                     base::Unretained(this)));
  NexusNetworkHost* host_ptr = host.get();
  in_flight_.push_back(std::move(host));
  host_ptr->Start();
}

void NexusNetworkBridgeAdapter::OnHttpGetComplete(NexusNetworkHost* host) {
  // Lookup-and-erase. Linear scan is fine — the in-flight list is
  // bounded by the number of concurrent WebFetch / future-tool calls
  // a session can have outstanding (low single digits in practice).
  auto it = std::find_if(
      in_flight_.begin(), in_flight_.end(),
      [host](const std::unique_ptr<NexusNetworkHost>& candidate) {
        return candidate.get() == host;
      });
  if (it != in_flight_.end()) {
    in_flight_.erase(it);
  }
  // The host already fired `done` from inside its own completion
  // path before invoking this callback; we just clean up the
  // unique_ptr here.
}

}  // namespace nexus
