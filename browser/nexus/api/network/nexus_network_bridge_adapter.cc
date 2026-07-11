#include "custom_browser/browser/nexus/api/network/nexus_network_bridge_adapter.h"

#include <algorithm>
#include <utility>

#include "base/strings/stringprintf.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/storage_partition.h"
#include "custom_browser/browser/nexus/api/network/nexus_network_host.h"
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
  // Pre-flight validation that runs synchronously on UI before we
  // spin up any per-request state. Mirrors CC's WebFetch URL
  // validator — same length cap, same scheme allow-list, same
  // creds-stripping behaviour. Errors here come back fast and the
  // caller can render a helpful message.

  if (!url_loader_factory_) {
    std::move(done).Error(
        "NETWORK_ERROR",
        "Network capability not initialised: no URLLoaderFactory was "
        "acquired at construction (was the BrowserContext null?).");
    return;
  }

  if (input.url.empty()) {
    std::move(done).Error("INVALID_URL", "URL must be a non-empty string.");
    return;
  }
  if (input.url.size() > kMaxUrlLengthChars) {
    std::move(done).Error(
        "INVALID_URL",
        base::StringPrintf(
            "URL exceeds maximum length of %zu characters (got %zu).",
            kMaxUrlLengthChars, input.url.size()));
    return;
  }

  GURL gurl(input.url);
  if (!gurl.is_valid()) {
    std::move(done).Error(
        "INVALID_URL",
        base::StringPrintf("Could not parse URL: %s", input.url.c_str()));
    return;
  }
  if (!gurl.SchemeIsHTTPOrHTTPS()) {
    // `GURL::scheme()` returns std::string_view; materialise via a
    // temporary `std::string` for printf %s formatting.
    const std::string scheme(gurl.scheme());
    std::move(done).Error(
        "INVALID_URL",
        base::StringPrintf(
            "Only http and https schemes are supported (got %s).",
            scheme.c_str()));
    return;
  }
  if (gurl.has_username() || gurl.has_password()) {
    // Defense-in-depth: URLs with embedded credentials should not
    // leak through this surface. The C++ Chromium URLLoader would
    // strip these anyway in most paths, but rejecting up-front
    // makes the contract explicit.
    std::move(done).Error(
        "INVALID_URL",
        "URLs may not embed credentials (user:pass@host) for network.httpGet.");
    return;
  }

  // Construct a per-request host. The host owns its `SimpleURLLoader`
  // and fires the completion callback when the body lands (or the
  // request fails / times out). Captures a raw `this` plus the host
  // pointer for the lookup-and-erase in `OnHttpGetComplete`.
  auto host = std::make_unique<NexusNetworkHost>(
      url_loader_factory_, std::move(input), std::move(done),
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
