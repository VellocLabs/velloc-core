#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_NETWORK_NEXUS_NETWORK_BRIDGE_ADAPTER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_NETWORK_NEXUS_NETWORK_BRIDGE_ADAPTER_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "custom_browser/browser/nexus/api/llm/llm_raw_stream_policy.h"
#include "custom_browser/browser/nexus/api/network/network_api.gen.h"
#include "custom_browser/browser/nexus/api/network/nexus_http_byte_stream.h"
#include "custom_browser/browser/nexus/api/network/nexus_guest_request.h"
#include "custom_browser/browser/nexus/api/network/nexus_websocket_connection.h"
#include "custom_browser/browser/nexus/bridge_response_callback.h"

namespace blink {
class MessagePortDescriptor;
}

namespace content {
class BrowserContext;
}

class PrefService;

namespace network {
class SharedURLLoaderFactory;
}

namespace network::mojom {
class NetworkContext;
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

  // The guest connection ended: every open network.stream* is cancelled.
  void OnBridgeConnectionClosed() override;

  // Most streams one connection may hold open.
  static constexpr size_t kMaxStreams = 32;
  // Most WebSockets one connection may hold open.
  static constexpr size_t kMaxSockets = 16;

  // Test seam: a factory that is not a profile's (TestURLLoaderFactory).
  void SetURLLoaderFactoryForTesting(
      scoped_refptr<network::SharedURLLoaderFactory> factory);
  size_t stream_count_for_testing() const { return streams_.size(); }
  // Test seam: the prefs the `{{secret:NAME}}` placeholders resolve from.
  void SetPrefsForTesting(PrefService* prefs) { prefs_ = prefs; }
  // Test seam: the network context WebSockets are created on.
  void SetNetworkContextForTesting(network::mojom::NetworkContext* context) {
    network_context_for_testing_ = context;
  }
  size_t socket_count_for_testing() const { return sockets_.size(); }

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

  // `network.streamOpen` / `streamRead` / `streamCancel`: general streaming
  // HTTP over NexusHttpByteStream (the core `model.rawStream*` shares),
  // under the egress policy, with no response-size cap by default.
  void HandleStreamOpenOnUI(
      ::nexus::generated::NetworkStreamOpenInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleStreamReadOnUI(
      ::nexus::generated::NetworkStreamReadInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleStreamCancelOnUI(
      ::nexus::generated::NetworkStreamCancelInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void OnStreamOpened(std::string stream_id,
                      GuestVisibleUrl visible_url,
                      ::nexus::BridgeResponseCallback done,
                      NexusHttpByteStream::OpenResult result);
  // Erase after the current callback frame (the stream may be on the stack).
  void EraseStreamSoon(const std::string& stream_id);

  // `network.wsOpen` / `wsSend` / `wsClose` and the onWsMessage / onWsClose
  // events, over NexusWebSocketConnection.
  void HandleWsOpenOnUI(
      ::nexus::generated::NetworkWsOpenInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleWsSendOnUI(
      ::nexus::generated::NetworkWsSendInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void HandleWsCloseOnUI(
      ::nexus::generated::NetworkWsCloseInput input,
      std::vector<blink::MessagePortDescriptor> ports,
      ::nexus::BridgeResponseCallback done) override;
  void OnWsOpened(std::string socket_id,
                  ::nexus::BridgeResponseCallback done,
                  std::string error_code,
                  std::string error_message,
                  std::string protocol);
  void OnWsMessage(std::string socket_id, bool binary, std::string data);
  void OnWsClosed(std::string socket_id,
                  uint16_t code,
                  std::string reason,
                  bool was_clean);
  // Erase after the current callback frame (the socket is on the stack).
  void EraseSocketSoon(const std::string& socket_id);
  network::mojom::NetworkContext* GetNetworkContext();

  // `{{oauth:NAME}}` with a providerId (httpRequest / streamOpen): the
  // provider-domain anchor, then the host's refresh / exchange
  // (ResolveProviderOAuthValues). On failure the callback gets the bridge
  // error code and message.
  using OAuthResolvedCallback =
      base::OnceCallback<void(std::optional<OAuthValues> values,
                              std::string error_code,
                              std::string error)>;
  void ResolveOAuthForRequest(const std::optional<std::string>& provider_id,
                              const std::string& url,
                              OAuthResolvedCallback callback);
  // The bodies of httpRequest / streamOpen once any OAuth values are known.
  void StartHttpRequest(::nexus::generated::HttpRequestInput input,
                        ::nexus::BridgeResponseCallback done,
                        std::optional<OAuthValues> oauth);
  void StartStreamOpen(::nexus::generated::NetworkStreamOpenInput input,
                       ::nexus::BridgeResponseCallback done,
                       std::optional<OAuthValues> oauth);

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

  // Where WebSockets are created (the profile's default storage partition).
  raw_ptr<content::BrowserContext> browser_context_ = nullptr;
  raw_ptr<network::mojom::NetworkContext> network_context_for_testing_ =
      nullptr;

  // The profile's prefs: where `{{secret:NAME}}` placeholders resolve
  // (NexusSecretStore). Null without a profile — a placeholder then fails
  // SECRET_UNAVAILABLE.
  raw_ptr<PrefService> prefs_ = nullptr;

  // Active in-flight requests. Inserted in `HandleHttpGetOnUi`,
  // removed in `OnHttpGetComplete`. The raw `NexusNetworkHost*`
  // returned by `host_ptr` is stable for the host's lifetime —
  // it's the key into this map.
  std::vector<std::unique_ptr<NexusNetworkHost>> in_flight_;

  // Open network.stream* streams by id.
  std::map<std::string, std::unique_ptr<NexusHttpByteStream>> streams_;
  // Ids cancelled before their open arrived (the body rides its own port,
  // so a cancel can overtake the open).
  RecentIdSet cancelled_stream_ids_;

  // Open (or opening) network.ws* sockets by id.
  std::map<std::string, std::unique_ptr<NexusWebSocketConnection>> sockets_;

  base::WeakPtrFactory<NexusNetworkBridgeAdapter> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_NETWORK_NEXUS_NETWORK_BRIDGE_ADAPTER_H_
