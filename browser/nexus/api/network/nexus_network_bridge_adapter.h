#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_NETWORK_NEXUS_NETWORK_BRIDGE_ADAPTER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_NETWORK_NEXUS_NETWORK_BRIDGE_ADAPTER_H_

#include <memory>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "custom_browser/browser/nexus/api/network/network_api.gen.h"
#include "custom_browser/browser/nexus/bridge_response_callback.h"

namespace blink {
class MessagePortDescriptor;
}

namespace content {
class BrowserContext;
}

namespace network {
class SharedURLLoaderFactory;
}

namespace nexus {

class NexusBridgeResponseDelegate;
class NexusNetworkHost;
struct NexusHttpRequestSpec;

// Concrete adapter implementing the generated `NetworkApiBase`
// virtuals. Adds a general-purpose HTTP surface (`httpGet`, and
// `httpRequest` for any method) to the bridge so
// TS-side tools (WebFetch in V1; future MCP-server pinging /
// webhook-callback / registry-lookup tools) can issue requests
// through the Chromium net stack — NOT subject to renderer CORS,
// same egress path the LLM providers use.
//
// Threading model — mirrors `NexusFsBridgeAdapter`:
//   - Per *.api.json declares its inputs in tools-style `inputs[]`.
//   - The codegen-emitted dispatcher handles typed input extraction
//     and PostTasks the entry hop to the declared thread (UI for
//     this category, since `SimpleURLLoader` and the
//     `SharedURLLoaderFactory` are UI-thread objects).
//   - Each Handle*OnUi runs on UI, sees a fully-populated typed
//     input, kicks off a `NexusNetworkHost` request that owns its
//     own `SimpleURLLoader`, and fires `done` from
//     `OnDownloadComplete` once the body lands. The
//     `BridgeResponseCallback` marshals the actual response payload
//     back to the JS guest.
//
// Lifetime: `NexusNetworkBridgeAdapter` owns each in-flight
// `NexusNetworkHost` via a per-request `unique_ptr` that
// self-erases on completion. Cancelling the adapter (or shutting
// down the parent service) drops every host, which destroys its
// loader, which cancels the in-flight request at the network layer.
class NexusNetworkBridgeAdapter
    : public ::nexus::generated::NetworkApiBase {
 public:
  NexusNetworkBridgeAdapter(NexusBridgeResponseDelegate* delegate,
                            content::BrowserContext* browser_context);
  ~NexusNetworkBridgeAdapter() override;

  NexusNetworkBridgeAdapter(const NexusNetworkBridgeAdapter&) = delete;
  NexusNetworkBridgeAdapter& operator=(const NexusNetworkBridgeAdapter&) =
      delete;

 private:
  // Override of generated::NetworkApiBase pure virtual. Runs on UI.
  // Validates the input URL (length, scheme, no embedded creds),
  // builds a `NexusNetworkHost`, hands the request off to it. The
  // host completes asynchronously and fires `done` from its
  // download-complete callback.
  void HandleHttpGetOnUI(
      ::nexus::generated::HttpGetInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  // `network.httpRequest`: the general surface — any method, an optional
  // body — under the same URL validation and the egress policy. Used by
  // the plugin `$.http.fetch` noun and the settings `http` hooks.
  void HandleHttpRequestOnUI(
      ::nexus::generated::HttpRequestInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;

  // Shared tail of both handlers: build the per-request host and start it.
  void StartRequest(NexusHttpRequestSpec spec,
                    ::nexus::BridgeResponseCallback done);

  // Per-request host completion. Looks up the host by its raw
  // pointer, fires the response (already wrapped in `done`), erases
  // the unique_ptr — which destroys the loader. The host is
  // self-cleaning: callers should never invalidate the pointer
  // outside this path.
  void OnHttpGetComplete(NexusNetworkHost* host);

  // Acquired once at construction from the profile's default storage
  // partition. Outlives every per-request host.
  scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory_;

  // Active in-flight requests. Inserted in `HandleHttpGetOnUi`,
  // removed in `OnHttpGetComplete`. The raw `NexusNetworkHost*`
  // returned by `host_ptr` is stable for the host's lifetime —
  // it's the key into this map.
  std::vector<std::unique_ptr<NexusNetworkHost>> in_flight_;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_NETWORK_NEXUS_NETWORK_BRIDGE_ADAPTER_H_
