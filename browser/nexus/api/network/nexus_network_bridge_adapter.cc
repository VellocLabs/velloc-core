#include "custom_browser/browser/nexus/api/network/nexus_network_bridge_adapter.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "base/base64.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/uuid.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/storage_partition.h"
#include "custom_browser/browser/nexus/api/network/network_egress_policy.h"
#include "custom_browser/browser/nexus/api/network/nexus_guest_request.h"
#include "custom_browser/browser/nexus/api/network/nexus_network_host.h"
#include "custom_browser/browser/nexus/api/llm/llm_raw_stream_policy.h"
#include "custom_browser/browser/nexus/api/llm/nexus_provider_oauth.h"
#include "custom_browser/browser/nexus/api/secrets/nexus_secret_store.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "net/base/url_util.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_util.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "url/gurl.h"
#include "url/origin.h"

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

// network.stream*: a stream's URL may be longer than a fetch's (signed
// download URLs), matching model.rawStreamOpen.
constexpr size_t kMaxStreamUrlChars = 8192;
constexpr int kDefaultStreamIdleTimeoutMs = 90'000;
constexpr int kDefaultStreamReadMaxBytes = 1024 * 1024;

constexpr net::NetworkTrafficAnnotationTag kStreamTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("nexus_network_stream", R"(
      semantics {
        sender: "Nexus AI Agent Platform"
        description:
          "A streaming HTTP request the Nexus web runtime makes through "
          "the browser's network stack (network.streamOpen): a model "
          "provider's API whose wire format the runtime implements, a "
          "plugin's or MCP server's HTTP endpoint, a large download. The "
          "response is streamed back to the runtime unparsed. Only "
          "publicly routable hosts are reached (network_egress_policy.h) "
          "unless the person opted a request out."
        trigger:
          "The AI assistant or a feature the user enabled (a provider, a "
          "plugin, an MCP server) issues a request."
        data:
          "The request the runtime composed: URL, headers and body, and "
          "any secret the user stored for that request's origin."
        destination: OTHER
        internal {
          contacts {
            email: "niclas@niclas.cc"
          }
        }
        user_data {
          type: USER_CONTENT
        }
        last_reviewed: "2026-10-04"
      }
      policy {
        cookies_allowed: NO
        setting:
          "Users configure providers, plugins and MCP servers in the "
          "assistant settings."
        policy_exception_justification:
          "Not yet covered by enterprise policy."
      }
    )");

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
                        ::nexus::BridgeResponseCallback& done,
                        size_t max_url_chars = kMaxUrlLengthChars) {
  if (url.empty()) {
    std::move(done).Error("INVALID_URL", "URL must be a non-empty string.");
    return false;
  }
  if (url.size() > max_url_chars) {
    std::move(done).Error(
        "INVALID_URL",
        base::StringPrintf(
            "URL exceeds maximum length of %zu characters (got %zu).",
            max_url_chars, url.size()));
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

// Expands `{{secret:NAME}}` in every string value of `values` (headers or
// query parameters) for a request to `url`: a secret is substituted only
// when `url`'s origin is the one it is bound to (NexusSecretStore). Names
// of entries that carried a secret are appended to `expanded_names`. Header
// values are re-validated after expansion so a secret with a CR/LF cannot
// split the header block. Returns false after answering `done`.
bool HasAnyTemplate(const std::string& text) {
  return HasSecretPlaceholder(text) || HasOAuthTemplate(text);
}

// Whether `values` carry an `{{oauth:NAME}}` (the providerId form).
bool NeedsOAuth(const std::optional<base::DictValue>& values) {
  if (!values.has_value()) {
    return false;
  }
  for (const auto [name, value] : *values) {
    if (value.is_string() && HasOAuthTemplate(value.GetString())) {
      return true;
    }
  }
  return false;
}

bool ExpandSecretsInValues(PrefService* prefs,
                           const GURL& url,
                           bool are_headers,
                           base::DictValue& values,
                           std::vector<std::string>* expanded_names,
                           ::nexus::BridgeResponseCallback& done,
                           const OAuthValues* oauth = nullptr) {
  bool any = false;
  for (const auto [name, value] : values) {
    if (value.is_string() && HasAnyTemplate(value.GetString())) {
      any = true;
      break;
    }
  }
  if (!any) {
    return true;
  }
  NexusSecretStore store(prefs);
  const NamespacedSecretLookup lookup =
      store.LookupFor(url::Origin::Create(url));
  // {{oauth:account_id}} with no account id: the entry is dropped, not sent
  // empty (IsOmittedOAuthValue). Removed after the walk.
  std::vector<std::string> omitted;
  for (auto [name, value] : values) {
    if (!value.is_string() || !HasAnyTemplate(value.GetString())) {
      continue;
    }
    // OAuth values first (only present on the providerId form, after the
    // host's refresh / exchange ran), then the stored secrets.
    std::string with_oauth;
    if (std::optional<std::string> oauth_error =
            ExpandOAuthTemplates(value.GetString(), oauth, &with_oauth)) {
      std::move(done).Error("OAUTH_UNAVAILABLE", std::move(*oauth_error));
      return false;
    }
    std::string expanded;
    if (std::optional<SecretExpansionError> error =
            ExpandSecretPlaceholders(with_oauth, lookup, &expanded)) {
      if (error->code == "INVALID_ARGS") {
        std::move(done).InvalidArgs(std::move(error->message));
      } else {
        std::move(done).Error(std::move(error->code),
                              std::move(error->message));
      }
      return false;
    }
    if (IsOmittedOAuthValue(value.GetString(), expanded)) {
      omitted.push_back(name);
      continue;
    }
    if (are_headers && !net::HttpUtil::IsValidHeaderValue(expanded)) {
      std::move(done).InvalidArgs("Invalid header value after secret "
                                  "substitution: " + name);
      return false;
    }
    value = base::Value(std::move(expanded));
    expanded_names->push_back(name);
  }
  for (const std::string& name : omitted) {
    values.Remove(name);
  }
  return true;
}

// Refuses a header that may not go on the wire (InvalidGuestHeader: the
// browser CHECKs on it), and with `reject_reserved` one the loader owns
// (IsReservedNetworkRequestHeader); without it a reserved header is left for
// NexusNetworkHost to drop. Runs on the values that will be sent, i.e. after
// `{{secret:NAME}}` expansion.
bool ValidateGuestHeaders(const std::optional<base::DictValue>& headers,
                          bool reject_reserved,
                          ::nexus::BridgeResponseCallback& done) {
  if (!headers.has_value()) {
    return true;
  }
  for (const auto [name, value] : *headers) {
    if (!value.is_string()) {
      continue;
    }
    if (reject_reserved && IsReservedNetworkRequestHeader(name)) {
      std::move(done).InvalidArgs("Header not allowed: " + name);
      return false;
    }
    if (std::optional<std::string> refusal =
            InvalidGuestHeader(name, value.GetString())) {
      std::move(done).InvalidArgs(std::move(*refusal));
      return false;
    }
  }
  return true;
}

// What httpRequest and streamOpen send, built one way.
struct PreparedGuestRequest {
  // The URL on the wire: the validated URL plus the (expanded) query.
  GURL url;
  // The URL the guest is shown back (`finalUrl`): placeholders, not secrets.
  GuestVisibleUrl visible_url;
  // Headers a secret or OAuth value was substituted into.
  std::vector<std::string> secret_headers;
};

// `{{secret:NAME}}` / `{{oauth:NAME}}` expanded in header and query values for
// `base`'s origin only (the query is appended after `base` was validated, so a
// secret can only add to the query of an already-approved URL), then every
// header checked. False when `done` was answered.
bool PrepareGuestRequest(PrefService* prefs,
                         const GURL& base,
                         bool reject_reserved_headers,
                         std::optional<base::DictValue>& headers,
                         std::optional<base::DictValue>& query,
                         const OAuthValues* oauth,
                         ::nexus::BridgeResponseCallback& done,
                         PreparedGuestRequest* out) {
  if (headers.has_value() &&
      !ExpandSecretsInValues(prefs, base, /*are_headers=*/true, *headers,
                             &out->secret_headers, done, oauth)) {
    return false;
  }
  if (!ValidateGuestHeaders(headers, reject_reserved_headers, done)) {
    return false;
  }
  GURL url = base;
  GURL guest = base;
  if (query.has_value()) {
    const base::DictValue placeholders = query->Clone();
    std::vector<std::string> expanded_params;
    if (!ExpandSecretsInValues(prefs, base, /*are_headers=*/false, *query,
                               &expanded_params, done, oauth)) {
      return false;
    }
    for (const auto [name, value] : *query) {
      if (!value.is_string()) {
        continue;
      }
      url = net::AppendQueryParameter(url, name, value.GetString());
      const std::string* placeholder = placeholders.FindString(name);
      guest = net::AppendQueryParameter(
          guest, name, placeholder ? *placeholder : value.GetString());
    }
    for (const std::string& name : expanded_params) {
      const std::string* placeholder = placeholders.FindString(name);
      const std::string* expanded = query->FindString(name);
      if (placeholder && expanded) {
        out->visible_url.AddExpansion(*placeholder, *expanded);
      }
    }
  }
  out->visible_url.SetUrls(url, guest);
  out->url = std::move(url);
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
  browser_context_ = browser_context;
  if (browser_context) {
    url_loader_factory_ = browser_context->GetDefaultStoragePartition()
                              ->GetURLLoaderFactoryForBrowserProcess();
    prefs_ = Profile::FromBrowserContext(browser_context)->GetPrefs();
  }
}

NexusNetworkBridgeAdapter::~NexusNetworkBridgeAdapter() = default;

void NexusNetworkBridgeAdapter::SetURLLoaderFactoryForTesting(
    scoped_refptr<network::SharedURLLoaderFactory> factory) {
  url_loader_factory_ = std::move(factory);
}

void NexusNetworkBridgeAdapter::OnBridgeConnectionClosed() {
  // Deleting a stream cancels its request (and answers a pending open or a
  // held read, whose callbacks the gone connection drops anyway).
  streams_.clear();
  // Dropping a socket closes it; nobody is left to tell.
  sockets_.clear();
}

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
  // httpGet keeps its pre-policy reach by default (WebFetch, marketplace
  // and skill downloads may name a local or LAN server, as in CC); a
  // caller opts into the policy with allowPrivateNetwork=false.
  if (!ValidateRequestUrl(input.url, input.allowPrivateNetwork.value_or(true),
                          done)) {
    return;
  }
  if (!ValidateGuestHeaders(input.headers, /*reject_reserved=*/false, done)) {
    return;
  }
  StartRequest(NexusHttpRequestSpec::FromHttpGet(std::move(input)),
               std::move(done));
}

void NexusNetworkBridgeAdapter::HandleHttpRequestOnUI(
    ::nexus::generated::HttpRequestInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  if (!NeedsOAuth(input.headers) && !NeedsOAuth(input.queryParams)) {
    StartHttpRequest(std::move(input), std::move(done), std::nullopt);
    return;
  }
  const std::optional<std::string> provider_id = input.providerId;
  const std::string url = input.url;
  ResolveOAuthForRequest(
      provider_id, url,
      base::BindOnce(
          [](base::WeakPtr<NexusNetworkBridgeAdapter> self,
             ::nexus::generated::HttpRequestInput input,
             ::nexus::BridgeResponseCallback done,
             std::optional<OAuthValues> values, std::string error_code,
             std::string error) {
            if (!values.has_value()) {
              std::move(done).Error(std::move(error_code), std::move(error));
              return;
            }
            if (self) {
              self->StartHttpRequest(std::move(input), std::move(done),
                                     std::move(values));
            }
          },
          weak_factory_.GetWeakPtr(), std::move(input), std::move(done)));
}

void NexusNetworkBridgeAdapter::ResolveOAuthForRequest(
    const std::optional<std::string>& provider_id,
    const std::string& url,
    OAuthResolvedCallback callback) {
  if (!provider_id.has_value() || provider_id->empty()) {
    std::move(callback).Run(std::nullopt, "INVALID_ARGS",
                            "{{oauth:NAME}} needs a providerId.");
    return;
  }
  Profile* profile =
      browser_context_ ? Profile::FromBrowserContext(browser_context_)
                       : nullptr;
  // The same anchor as model.rawStreamOpen: the request must stay on the
  // provider's registrable domain.
  if (std::optional<std::string> refusal = CheckRawStreamTarget(
          GURL(GetProviderAnchorBaseUrl(profile, *provider_id)), GURL(url))) {
    std::move(callback).Run(std::nullopt, "TARGET_NOT_ALLOWED",
                            std::move(*refusal));
    return;
  }
  ResolveProviderOAuthValues(
      profile, *provider_id,
      base::BindOnce(
          [](OAuthResolvedCallback callback, std::optional<OAuthValues> values,
             std::string error) {
            std::move(callback).Run(std::move(values), "OAUTH_UNAVAILABLE",
                                    std::move(error));
          },
          std::move(callback)));
}

void NexusNetworkBridgeAdapter::StartHttpRequest(
    ::nexus::generated::HttpRequestInput input,
    ::nexus::BridgeResponseCallback done,
    std::optional<OAuthValues> oauth) {
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
  // An optional `bytes` input is generated as a plain vector: empty when
  // absent.
  if (!input.body.empty()) {
    if (method == "GET" || method == "HEAD") {
      std::move(done).Error("INVALID_REQUEST",
                            "A GET or HEAD request may not carry a body.");
      return;
    }
    if (input.body.size() > kMaxRequestBodyBytes) {
      std::move(done).Error(
          "REQUEST_TOO_LARGE",
          base::StringPrintf("Request body of %zu bytes exceeds the cap of %zu.",
                             input.body.size(), kMaxRequestBodyBytes));
      return;
    }
  }
  // A reserved header is dropped by the host on this surface (its
  // pre-existing contract), refused on streamOpen / wsOpen.
  PreparedGuestRequest prepared;
  if (!PrepareGuestRequest(prefs_, GURL(input.url),
                           /*reject_reserved_headers=*/false, input.headers,
                           input.queryParams, oauth ? &*oauth : nullptr, done,
                           &prepared)) {
    return;
  }
  NexusHttpRequestSpec spec =
      NexusHttpRequestSpec::FromHttpRequest(std::move(input));
  spec.url = prepared.url.spec();
  spec.visible_url = std::move(prepared.visible_url);
  if (!prepared.secret_headers.empty()) {
    spec.secret_headers = std::move(prepared.secret_headers);
    spec.secret_origin = url::Origin::Create(prepared.url).Serialize();
  }
  StartRequest(std::move(spec), std::move(done));
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

// ===========================================================================
// network.streamOpen / streamRead / streamCancel
// ===========================================================================

void NexusNetworkBridgeAdapter::HandleStreamOpenOnUI(
    ::nexus::generated::NetworkStreamOpenInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  if (!NeedsOAuth(input.headers) && !NeedsOAuth(input.queryParams)) {
    StartStreamOpen(std::move(input), std::move(done), std::nullopt);
    return;
  }
  const std::optional<std::string> provider_id = input.providerId;
  const std::string url = input.url;
  ResolveOAuthForRequest(
      provider_id, url,
      base::BindOnce(
          [](base::WeakPtr<NexusNetworkBridgeAdapter> self,
             ::nexus::generated::NetworkStreamOpenInput input,
             ::nexus::BridgeResponseCallback done,
             std::optional<OAuthValues> values, std::string error_code,
             std::string error) {
            if (!values.has_value()) {
              std::move(done).Error(std::move(error_code), std::move(error));
              return;
            }
            if (self) {
              self->StartStreamOpen(std::move(input), std::move(done),
                                    std::move(values));
            }
          },
          weak_factory_.GetWeakPtr(), std::move(input), std::move(done)));
}

void NexusNetworkBridgeAdapter::StartStreamOpen(
    ::nexus::generated::NetworkStreamOpenInput input,
    ::nexus::BridgeResponseCallback done,
    std::optional<OAuthValues> oauth) {
  if (!url_loader_factory_) {
    std::move(done).Error("NETWORK_ERROR",
                          "Network capability not initialised.");
    return;
  }
  if (streams_.size() >= kMaxStreams) {
    std::move(done).Error("TOO_MANY_STREAMS", "Too many streams are open.");
    return;
  }
  if (input.streamId.has_value()) {
    const std::string& client_id = *input.streamId;
    if (!IsValidClientStreamId(client_id)) {
      std::move(done).InvalidArgs(
          "streamId must be 8-64 chars of [A-Za-z0-9_-].");
      return;
    }
    if (streams_.contains(client_id)) {
      std::move(done).InvalidArgs("stream id in use: " + client_id);
      return;
    }
    if (cancelled_stream_ids_.Take(client_id)) {
      std::move(done).Error(NexusHttpByteStream::kCancelledErrorCode,
                            "Stream was cancelled before it opened.");
      return;
    }
  }

  const bool allow_private = input.allowPrivateNetwork.value_or(false);
  if (!ValidateRequestUrl(input.url, allow_private, done,
                          kMaxStreamUrlChars)) {
    return;
  }
  const std::string method = base::ToUpperASCII(input.method.value_or("GET"));
  if (method.empty() || !net::HttpUtil::IsValidHeaderName(method) ||
      IsForbiddenMethod(method)) {
    std::move(done).Error(
        "INVALID_METHOD",
        base::StringPrintf("Unsupported HTTP method: %s", method.c_str()));
    return;
  }
  if (!input.body.empty() && (method == "GET" || method == "HEAD")) {
    std::move(done).Error("INVALID_REQUEST",
                          "A GET or HEAD request may not carry a body.");
    return;
  }
  if (input.body.size() > kMaxRequestBodyBytes) {
    std::move(done).Error(
        "REQUEST_TOO_LARGE",
        base::StringPrintf("Request body of %zu bytes exceeds the cap of %zu.",
                           input.body.size(), kMaxRequestBodyBytes));
    return;
  }

  PreparedGuestRequest prepared;
  if (!PrepareGuestRequest(prefs_, GURL(input.url),
                           /*reject_reserved_headers=*/true, input.headers,
                           input.queryParams, oauth ? &*oauth : nullptr, done,
                           &prepared)) {
    return;
  }
  const url::Origin secret_origin = url::Origin::Create(prepared.url);

  auto request = std::make_unique<network::ResourceRequest>();
  request->url = prepared.url;
  request->method = method;
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  std::string content_type = "application/octet-stream";
  if (input.headers.has_value()) {
    for (const auto [name, value] : *input.headers) {
      if (!value.is_string()) {
        continue;
      }
      // Checked in PrepareGuestRequest.
      const std::string& header_value = value.GetString();
      if (base::EqualsCaseInsensitiveASCII(
              name, net::HttpRequestHeaders::kContentType)) {
        content_type = header_value;
        continue;  // AttachStringForUpload sets it.
      }
      request->headers.SetHeader(name, header_value);
    }
  }

  const int idle_ms = input.idleTimeoutMs.value_or(0) > 0
                          ? *input.idleTimeoutMs
                          : kDefaultStreamIdleTimeoutMs;
  size_t max_bytes = std::numeric_limits<size_t>::max();
  if (input.maxResponseBytes.has_value() && *input.maxResponseBytes > 0) {
    max_bytes = static_cast<size_t>(*input.maxResponseBytes);
  }

  auto stream = std::make_unique<NexusHttpByteStream>(
      url_loader_factory_, std::move(request),
      std::string(input.body.begin(), input.body.end()),
      std::move(content_type), max_bytes, base::Milliseconds(idle_ms),
      // A hop that leaves the secrets' origin drops the headers they went
      // into (a no-op when none did).
      base::BindRepeating(
          [](const url::Origin& origin, const GURL& next) {
            return origin.IsSameOriginWith(next);
          },
          secret_origin),
      std::move(prepared.secret_headers), NexusHttpByteStream::BodyTransform(),
      kStreamTrafficAnnotation);
  if (!allow_private) {
    stream->EnableEgressPolicy();
  }
  const std::string stream_id =
      input.streamId.has_value()
          ? *input.streamId
          : base::Uuid::GenerateRandomV4().AsLowercaseString();
  NexusHttpByteStream* stream_ptr = stream.get();
  streams_[stream_id] = std::move(stream);
  stream_ptr->Start(base::BindOnce(&NexusNetworkBridgeAdapter::OnStreamOpened,
                                   weak_factory_.GetWeakPtr(), stream_id,
                                   std::move(prepared.visible_url),
                                   std::move(done)));
}

void NexusNetworkBridgeAdapter::OnStreamOpened(
    std::string stream_id,
    GuestVisibleUrl visible_url,
    ::nexus::BridgeResponseCallback done,
    NexusHttpByteStream::OpenResult result) {
  if (!result.error_code.empty()) {
    // CANCELLED: the stream was deleted (streamCancel already removed it).
    if (result.error_code != NexusHttpByteStream::kCancelledErrorCode) {
      EraseStreamSoon(stream_id);
    }
    std::move(done).Error(std::move(result.error_code),
                          std::move(result.error_message));
    return;
  }
  base::DictValue out;
  out.Set("streamId", stream_id);
  out.Set("status", result.status);
  out.Set("statusText", std::move(result.status_text));
  // Never the expanded {{secret:NAME}} / {{oauth:NAME}} query values.
  out.Set("finalUrl", visible_url.For(GURL(result.final_url)));
  out.Set("headers", std::move(result.headers));
  std::move(done).Success(std::move(out));
}

void NexusNetworkBridgeAdapter::HandleStreamReadOnUI(
    ::nexus::generated::NetworkStreamReadInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  auto it = streams_.find(input.streamId);
  if (it == streams_.end()) {
    std::move(done).Error("STREAM_UNKNOWN",
                          "Unknown stream: " + input.streamId);
    return;
  }
  const int max_bytes = input.maxBytes.value_or(0) > 0
                            ? *input.maxBytes
                            : kDefaultStreamReadMaxBytes;
  it->second->Read(
      base::Milliseconds(std::max(0, input.waitMs.value_or(0))),
      static_cast<size_t>(max_bytes),
      base::BindOnce(
          [](base::WeakPtr<NexusNetworkBridgeAdapter> self,
             std::string stream_id, ::nexus::BridgeResponseCallback done,
             NexusHttpByteStream::ReadResult result) {
            if (result.done && self &&
                result.error_code != NexusHttpByteStream::kCancelledErrorCode) {
              self->EraseStreamSoon(stream_id);
            }
            base::DictValue envelope;
            envelope.Set("done", result.done);
            if (!result.error_code.empty()) {
              envelope.Set("errorCode", std::move(result.error_code));
              envelope.Set("errorMessage", std::move(result.error_message));
            }
            std::move(done).Binary(
                base::Value(std::move(envelope)),
                mojo_base::BigBuffer(base::as_byte_span(result.data)),
                /*binary_field=*/"chunk");
          },
          weak_factory_.GetWeakPtr(), input.streamId, std::move(done)));
}

void NexusNetworkBridgeAdapter::HandleStreamCancelOnUI(
    ::nexus::generated::NetworkStreamCancelInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  {
    auto node = streams_.extract(input.streamId);
    if (node.empty()) {
      cancelled_stream_ids_.Insert(input.streamId);
    }
  }  // the stream (if any) dies here, answering its pending open / read
  std::move(done).SuccessVoid();
}

void NexusNetworkBridgeAdapter::EraseStreamSoon(const std::string& stream_id) {
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<NexusNetworkBridgeAdapter> self, std::string id) {
            if (self) {
              self->streams_.erase(id);
            }
          },
          weak_factory_.GetWeakPtr(), stream_id));
}

// ===========================================================================
// network.wsOpen / wsSend / wsClose
// ===========================================================================

network::mojom::NetworkContext* NexusNetworkBridgeAdapter::GetNetworkContext() {
  if (network_context_for_testing_) {
    return network_context_for_testing_;
  }
  return browser_context_
             ? browser_context_->GetDefaultStoragePartition()
                   ->GetNetworkContext()
             : nullptr;
}

void NexusNetworkBridgeAdapter::HandleWsOpenOnUI(
    ::nexus::generated::NetworkWsOpenInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  network::mojom::NetworkContext* context = GetNetworkContext();
  if (!context) {
    std::move(done).Error("NETWORK_ERROR",
                          "Network capability not initialised.");
    return;
  }
  if (sockets_.size() >= kMaxSockets) {
    std::move(done).Error("TOO_MANY_SOCKETS", "Too many sockets are open.");
    return;
  }
  if (input.socketId.has_value()) {
    if (!IsValidClientStreamId(*input.socketId)) {
      std::move(done).InvalidArgs(
          "socketId must be 8-64 chars of [A-Za-z0-9_-].");
      return;
    }
    if (sockets_.contains(*input.socketId)) {
      std::move(done).InvalidArgs("socket id in use: " + *input.socketId);
      return;
    }
  }

  const GURL url(input.url);
  if (input.url.empty() || input.url.size() > kMaxStreamUrlChars ||
      !url.is_valid() || !url.SchemeIsWSOrWSS() || url.has_username() ||
      url.has_password()) {
    std::move(done).Error(
        "INVALID_URL", "A WebSocket URL is ws:// or wss://, at most 8192 "
                       "chars, without credentials.");
    return;
  }
  // The http(s) twin of the URL: the egress literal-host check and the
  // secrets' origin binding are both expressed for http(s).
  GURL::Replacements to_http;
  to_http.SetSchemeStr(url.SchemeIs("wss") ? "https" : "http");
  const GURL http_url = url.ReplaceComponents(to_http);
  const bool allow_private = input.allowPrivateNetwork.value_or(false);
  if (!allow_private) {
    if (std::optional<std::string> refusal =
            EgressRefusalForLiteralHost(http_url)) {
      std::move(done).Error(
          "EGRESS_BLOCKED",
          base::StringPrintf("Refused by the egress policy: %s.",
                             refusal->c_str()));
      return;
    }
  }

  std::vector<network::mojom::HttpHeaderPtr> headers;
  if (input.headers.has_value()) {
    std::vector<std::string> secret_headers;
    if (!ExpandSecretsInValues(prefs_, http_url, /*are_headers=*/true,
                               *input.headers, &secret_headers, done) ||
        !ValidateGuestHeaders(input.headers, /*reject_reserved=*/true, done)) {
      return;
    }
    for (const auto [name, value] : *input.headers) {
      if (!value.is_string()) {
        continue;
      }
      headers.push_back(
          network::mojom::HttpHeader::New(name, value.GetString()));
    }
  }

  const std::string socket_id =
      input.socketId.has_value()
          ? *input.socketId
          : base::Uuid::GenerateRandomV4().AsLowercaseString();
  auto socket = std::make_unique<NexusWebSocketConnection>(
      context, url, input.protocols.value_or(std::vector<std::string>()),
      std::move(headers), !allow_private,
      base::BindOnce(&NexusNetworkBridgeAdapter::OnWsOpened,
                     weak_factory_.GetWeakPtr(), socket_id, std::move(done)),
      base::BindRepeating(&NexusNetworkBridgeAdapter::OnWsMessage,
                          weak_factory_.GetWeakPtr(), socket_id),
      base::BindOnce(&NexusNetworkBridgeAdapter::OnWsClosed,
                     weak_factory_.GetWeakPtr(), socket_id));
  NexusWebSocketConnection* socket_ptr = socket.get();
  sockets_[socket_id] = std::move(socket);
  socket_ptr->Start();
}

void NexusNetworkBridgeAdapter::OnWsOpened(std::string socket_id,
                                           ::nexus::BridgeResponseCallback done,
                                           std::string error_code,
                                           std::string error_message,
                                           std::string protocol) {
  if (!error_code.empty()) {
    EraseSocketSoon(socket_id);
    std::move(done).Error(std::move(error_code), std::move(error_message));
    return;
  }
  base::DictValue out;
  out.Set("socketId", socket_id);
  out.Set("protocol", std::move(protocol));
  std::move(done).Success(std::move(out));
}

void NexusNetworkBridgeAdapter::OnWsMessage(std::string socket_id,
                                            bool binary,
                                            std::string data) {
  base::DictValue route;
  route.Set("namespace", "nexus");
  route.Set("capability", "network");
  route.Set("action", "onWsMessage");
  base::DictValue payload;
  payload.Set("socketId", std::move(socket_id));
  if (binary || !base::IsStringUTF8AllowingNoncharacters(data)) {
    // base::Value strings must be UTF-8; a text frame that is not (the
    // server's bug) travels as bytes rather than being dropped.
    payload.Set("dataBase64", base::Base64Encode(data));
  } else {
    payload.Set("text", std::move(data));
  }
  SendEvent(route, base::Value(std::move(payload)));
}

void NexusNetworkBridgeAdapter::OnWsClosed(std::string socket_id,
                                           uint16_t code,
                                           std::string reason,
                                           bool was_clean) {
  base::DictValue route;
  route.Set("namespace", "nexus");
  route.Set("capability", "network");
  route.Set("action", "onWsClose");
  base::DictValue payload;
  payload.Set("socketId", socket_id);
  payload.Set("code", static_cast<int>(code));
  payload.Set("reason", base::IsStringUTF8AllowingNoncharacters(reason)
                            ? std::move(reason)
                            : std::string());
  payload.Set("wasClean", was_clean);
  SendEvent(route, base::Value(std::move(payload)));
  EraseSocketSoon(socket_id);
}

void NexusNetworkBridgeAdapter::HandleWsSendOnUI(
    ::nexus::generated::NetworkWsSendInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  if (input.text.has_value() == input.dataBase64.has_value()) {
    std::move(done).InvalidArgs("wsSend takes exactly one of text or "
                                "dataBase64.");
    return;
  }
  std::string data;
  bool binary = false;
  if (input.text.has_value()) {
    data = std::move(*input.text);
  } else {
    std::optional<std::vector<uint8_t>> decoded =
        base::Base64Decode(*input.dataBase64);
    if (!decoded.has_value()) {
      std::move(done).InvalidArgs("dataBase64 is not valid base64.");
      return;
    }
    data.assign(decoded->begin(), decoded->end());
    binary = true;
  }
  if (data.size() > NexusWebSocketConnection::kMaxMessageBytes) {
    std::move(done).Error("MESSAGE_TOO_LARGE",
                          "A message is at most 16 MiB.");
    return;
  }
  auto it = sockets_.find(input.socketId);
  const bool sent =
      it != sockets_.end() && it->second->Send(binary, std::move(data));
  base::DictValue out;
  out.Set("sent", sent);
  std::move(done).Success(std::move(out));
}

void NexusNetworkBridgeAdapter::HandleWsCloseOnUI(
    ::nexus::generated::NetworkWsCloseInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  auto it = sockets_.find(input.socketId);
  const bool found = it != sockets_.end();
  if (found) {
    const int code = input.code.value_or(1000);
    it->second->Close(static_cast<uint16_t>(std::clamp(code, 1000, 4999)),
                      input.reason.value_or(std::string()));
  }
  base::DictValue out;
  out.Set("found", found);
  std::move(done).Success(std::move(out));
}

void NexusNetworkBridgeAdapter::EraseSocketSoon(const std::string& socket_id) {
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<NexusNetworkBridgeAdapter> self, std::string id) {
            if (self) {
              self->sockets_.erase(id);
            }
          },
          weak_factory_.GetWeakPtr(), socket_id));
}

}  // namespace nexus
