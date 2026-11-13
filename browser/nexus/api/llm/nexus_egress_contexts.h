// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_NEXUS_EGRESS_CONTEXTS_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_NEXUS_EGRESS_CONTEXTS_H_

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "base/functional/callback.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/supports_user_data.h"
#include "base/time/time.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "net/proxy_resolution/proxy_config_with_annotation.h"
#include "services/network/public/mojom/network_context.mojom-forward.h"

namespace content {
class BrowserContext;
}

namespace network {
class SharedURLLoaderFactory;
}

namespace nexus {

struct ProxyEndpoint;

// The network contexts provider requests leave through when they name an
// egress (P-4). A proxy is a property of a network context, so "this
// request through that proxy" is "this request on that proxy's context".
//
// Contract: one context per proxy server and credentials, made on first use
// and shared by every request that names the same proxy. A context with no
// request on it is dropped after kIdleLifetime. At most kMaxContexts exist;
// past that an idle one is dropped to make room, and when every context is
// carrying a request the next new proxy is refused. A context whose
// connection to the network service was lost is made again on next use.
// Lives on the UI thread, owned by its browser context.
class NexusEgressContexts : public base::SupportsUserData::Data {
 public:
  static constexpr size_t kMaxContexts = 16;
  static constexpr base::TimeDelta kIdleLifetime = base::Minutes(5);

  // One request's hold on a context. The context stays alive while `release`
  // has not run; destroying the lease runs it.
  struct Lease {
    Lease();
    Lease(Lease&&);
    Lease& operator=(Lease&&);
    ~Lease();

    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory;
    base::ScopedClosureRunner release;
  };

  // Binds a new network context whose only proxy configuration is `config`.
  using ContextBinder = base::RepeatingCallback<void(
      mojo::PendingReceiver<network::mojom::NetworkContext>,
      const net::ProxyConfigWithAnnotation& config)>;

  // The instance of `browser_context`, created on first call.
  static NexusEgressContexts* GetForBrowserContext(
      content::BrowserContext* browser_context);

  NexusEgressContexts();
  ~NexusEgressContexts() override;

  NexusEgressContexts(const NexusEgressContexts&) = delete;
  NexusEgressContexts& operator=(const NexusEgressContexts&) = delete;

  // A loader factory on the context of `endpoint`, or nullopt when
  // kMaxContexts contexts are all carrying requests.
  std::optional<Lease> Acquire(const ProxyEndpoint& endpoint);

  void SetContextBinderForTesting(ContextBinder binder);
  size_t context_count_for_testing() const { return entries_.size(); }

 private:
  class Entry;

  void Release(const std::string& key);
  // Drops every context that has carried no request for kIdleLifetime.
  void DropIdle();
  // Drops the idle context used longest ago; false when none is idle.
  bool DropLeastRecentlyUsedIdle();

  ContextBinder binder_;
  std::map<std::string, std::unique_ptr<Entry>> entries_;
  base::WeakPtrFactory<NexusEgressContexts> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_NEXUS_EGRESS_CONTEXTS_H_
