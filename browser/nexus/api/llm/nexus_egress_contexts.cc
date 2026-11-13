// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/llm/nexus_egress_contexts.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/sequenced_task_runner.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/net/system_network_context_manager.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/network_service_instance.h"
#include "custom_browser/browser/nexus/api/llm/llm_egress_policy.h"
#include "custom_browser/browser/nexus/api/network/nexus_proxy_endpoint.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/base/auth.h"
#include "net/base/net_errors.h"
#include "net/base/proxy_string_util.h"
#include "net/proxy_resolution/proxy_config.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/wrapper_shared_url_loader_factory.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "services/network/public/mojom/url_loader_network_service_observer.mojom.h"

namespace nexus {

namespace {

const char kUserDataKey[] = "nexus_egress_contexts";

constexpr net::NetworkTrafficAnnotationTag kEgressProxyAnnotation =
    net::DefineNetworkTrafficAnnotation("nexus_provider_egress_proxy", R"(
        semantics {
          sender: "Nexus provider egress"
          description:
            "A model provider installed by the user can name a proxy server "
            "its requests are sent through. Requests that name one are sent "
            "on a network context whose proxy configuration is that server."
          trigger:
            "A request to a model provider whose configuration names a proxy."
          data: "The proxy server's address."
          destination: OTHER
          destination_other: "The proxy server the provider declared."
        }
        policy {
          cookies_allowed: NO
          setting:
            "Users choose which providers are installed, and approve the "
            "proxy hosts a provider declares when they install it."
          policy_exception_justification: "Not implemented."
        })");

void BindContextInNetworkService(
    mojo::PendingReceiver<network::mojom::NetworkContext> receiver,
    const net::ProxyConfigWithAnnotation& config) {
  network::mojom::NetworkContextParamsPtr params =
      SystemNetworkContextManager::GetInstance()
          ->CreateDefaultNetworkContextParams();
  params->http_cache_enabled = false;
  params->initial_proxy_config = config;
  content::CreateNetworkContextInNetworkService(std::move(receiver),
                                                std::move(params));
}

}  // namespace

// One proxy's network context, and the observer every loader factory made on
// it reports to.
class NexusEgressContexts::Entry
    : public network::mojom::URLLoaderNetworkServiceObserver {
 public:
  explicit Entry(const ProxyEndpoint& endpoint) : endpoint_(endpoint) {}
  ~Entry() override = default;

  Entry(const Entry&) = delete;
  Entry& operator=(const Entry&) = delete;

  scoped_refptr<network::SharedURLLoaderFactory> CreateFactory(
      const ContextBinder& binder) {
    if (!context_ || !context_.is_connected()) {
      context_.reset();
      observers_.Clear();
      net::ProxyConfig config;
      config.proxy_rules().ParseFromString(
          net::ProxyServerToProxyUri(endpoint_.server));
      binder.Run(context_.BindNewPipeAndPassReceiver(),
                 net::ProxyConfigWithAnnotation(config, kEgressProxyAnnotation));
    }
    // The parameters the profile's own browser-process factory is made with,
    // so a request behaves the same on either context.
    network::mojom::URLLoaderFactoryParamsPtr params =
        network::mojom::URLLoaderFactoryParams::New();
    params->process_id = network::OriginatingProcessId::browser();
    params->automatically_assign_isolation_info = true;
    params->is_orb_enabled = false;
    params->is_trusted = true;
    observers_.Add(
        this, params->url_loader_network_observer
                  .InitWithNewPipeAndPassReceiver());
    mojo::PendingRemote<network::mojom::URLLoaderFactory> factory;
    context_->CreateURLLoaderFactory(factory.InitWithNewPipeAndPassReceiver(),
                                     std::move(params));
    return base::MakeRefCounted<network::WrapperSharedURLLoaderFactory>(
        std::move(factory));
  }

  int leases = 0;
  base::TimeTicks last_used;

 private:
  // network::mojom::URLLoaderNetworkServiceObserver:
  void OnSSLCertificateError(const GURL& url,
                             int net_error,
                             const net::SSLInfo& ssl_info,
                             bool fatal,
                             OnSSLCertificateErrorCallback response) override {
    std::move(response).Run(net::ERR_INSECURE_RESPONSE);
  }
  void OnCertificateRequested(
      const std::optional<base::UnguessableToken>& window_id,
      const scoped_refptr<net::SSLCertRequestInfo>& cert_info,
      mojo::PendingRemote<network::mojom::ClientCertificateResponder>
          cert_responder) override {
    mojo::Remote<network::mojom::ClientCertificateResponder> responder(
        std::move(cert_responder));
    responder->ContinueWithoutCertificate();
  }
  // Security: only the proxy's own challenge is answered, and only once —
  // the stored credentials are never offered to the target server, and a
  // proxy that refuses them is not asked again with the same pair.
  void OnAuthRequired(
      const std::optional<base::UnguessableToken>& window_id,
      int32_t request_id,
      const GURL& url,
      bool first_auth_attempt,
      const net::AuthChallengeInfo& auth_info,
      const scoped_refptr<net::HttpResponseHeaders>& head_headers,
      mojo::PendingRemote<network::mojom::AuthChallengeResponder>
          auth_challenge_responder) override {
    mojo::Remote<network::mojom::AuthChallengeResponder> responder(
        std::move(auth_challenge_responder));
    if (auth_info.is_proxy && first_auth_attempt &&
        !endpoint_.username.empty()) {
      responder->OnAuthCredentials(
          net::AuthCredentials(base::UTF8ToUTF16(endpoint_.username),
                               base::UTF8ToUTF16(endpoint_.password)));
      return;
    }
    responder->OnAuthCredentials(std::nullopt);
  }
  void OnLocalNetworkAccessPermissionRequired(
      network::mojom::TransportType type,
      network::mojom::IPAddressSpace ip_address_space,
      OnLocalNetworkAccessPermissionRequiredCallback callback) override {
    std::move(callback).Run(network::mojom::LocalNetworkAccessResult::kDenied);
  }
  void OnPlatformLocalNetworkPermissionRequired(
      OnPlatformLocalNetworkPermissionRequiredCallback callback) override {
    std::move(callback).Run(/*granted=*/false);
  }
  void OnClearSiteData(
      const GURL& url,
      const std::string& header_value,
      int32_t load_flags,
      const std::optional<net::CookiePartitionKey>& cookie_partition_key,
      bool partitioned_state_allowed_only,
      OnClearSiteDataCallback callback) override {
    std::move(callback).Run();
  }
  void OnLoadingStateUpdate(network::mojom::LoadInfoPtr info,
                            OnLoadingStateUpdateCallback callback) override {
    std::move(callback).Run();
  }
  void OnDataUseUpdate(int32_t network_traffic_annotation_id_hash,
                       base::ByteSize recv_bytes,
                       base::ByteSize sent_bytes) override {}
  void Clone(
      mojo::PendingReceiver<network::mojom::URLLoaderNetworkServiceObserver>
          listener) override {
    observers_.Add(this, std::move(listener));
  }
  void OnWebSocketConnectedToLocalNetwork(
      const GURL& request_url,
      network::mojom::IPAddressSpace ip_address_space) override {}
  void OnUrlLoaderConnectedToLocalNetwork(
      const GURL& request_url,
      network::mojom::IPAddressSpace response_address_space,
      network::mojom::IPAddressSpace client_address_space,
      network::mojom::IPAddressSpace target_address_space) override {}

  const ProxyEndpoint endpoint_;
  mojo::Remote<network::mojom::NetworkContext> context_;
  mojo::ReceiverSet<network::mojom::URLLoaderNetworkServiceObserver> observers_;
};

NexusEgressContexts::Lease::Lease() = default;
NexusEgressContexts::Lease::Lease(Lease&&) = default;
NexusEgressContexts::Lease& NexusEgressContexts::Lease::operator=(Lease&&) =
    default;
NexusEgressContexts::Lease::~Lease() = default;

// static
NexusEgressContexts* NexusEgressContexts::GetForBrowserContext(
    content::BrowserContext* browser_context) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  auto* contexts = static_cast<NexusEgressContexts*>(
      browser_context->GetUserData(kUserDataKey));
  if (!contexts) {
    auto owned = std::make_unique<NexusEgressContexts>();
    contexts = owned.get();
    browser_context->SetUserData(kUserDataKey, std::move(owned));
  }
  return contexts;
}

NexusEgressContexts::NexusEgressContexts()
    : binder_(base::BindRepeating(&BindContextInNetworkService)) {}

NexusEgressContexts::~NexusEgressContexts() = default;

std::optional<NexusEgressContexts::Lease> NexusEgressContexts::Acquire(
    const ProxyEndpoint& endpoint) {
  DropIdle();
  const std::string key = EgressContextKey(endpoint);
  auto it = entries_.find(key);
  if (it == entries_.end()) {
    if (entries_.size() >= kMaxContexts && !DropLeastRecentlyUsedIdle()) {
      return std::nullopt;
    }
    it = entries_.emplace(key, std::make_unique<Entry>(endpoint)).first;
  }
  Entry& entry = *it->second;
  Lease lease;
  lease.url_loader_factory = entry.CreateFactory(binder_);
  ++entry.leases;
  entry.last_used = base::TimeTicks::Now();
  lease.release = base::ScopedClosureRunner(base::BindOnce(
      &NexusEgressContexts::Release, weak_factory_.GetWeakPtr(), key));
  return lease;
}

void NexusEgressContexts::SetContextBinderForTesting(ContextBinder binder) {
  binder_ = std::move(binder);
}

void NexusEgressContexts::Release(const std::string& key) {
  auto it = entries_.find(key);
  if (it == entries_.end()) {
    return;
  }
  Entry& entry = *it->second;
  if (entry.leases > 0) {
    --entry.leases;
  }
  entry.last_used = base::TimeTicks::Now();
  if (entry.leases == 0) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&NexusEgressContexts::DropIdle,
                       weak_factory_.GetWeakPtr()),
        kIdleLifetime);
  }
}

void NexusEgressContexts::DropIdle() {
  const base::TimeTicks now = base::TimeTicks::Now();
  std::erase_if(entries_, [now](const auto& item) {
    const Entry& entry = *item.second;
    return entry.leases == 0 && now - entry.last_used >= kIdleLifetime;
  });
}

bool NexusEgressContexts::DropLeastRecentlyUsedIdle() {
  auto oldest = entries_.end();
  for (auto it = entries_.begin(); it != entries_.end(); ++it) {
    if (it->second->leases != 0) {
      continue;
    }
    if (oldest == entries_.end() ||
        it->second->last_used < oldest->second->last_used) {
      oldest = it;
    }
  }
  if (oldest == entries_.end()) {
    return false;
  }
  entries_.erase(oldest);
  return true;
}

}  // namespace nexus
