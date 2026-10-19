// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_BOOT_BUNDLE_BOOT_BUNDLE_URL_LOADER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_BOOT_BUNDLE_BOOT_BUNDLE_URL_LOADER_H_

#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "services/network/public/mojom/url_loader_factory.mojom-forward.h"

namespace content {
class BrowserContext;
class RenderFrameHost;
}  // namespace content

namespace network {
class URLLoaderFactoryBuilder;
struct ResourceRequest;
}  // namespace network

namespace nexus {

class BootBundleStore;

// Contract (F-33): a GET for a bundled guest URL in the default or
// force-cache mode is first sent with only-from-cache flags, so a cached copy
// is returned exactly as before and the network is never touched. On a cache
// miss the bundled bytes answer it; when the bundle cannot, the original
// request goes to the network unchanged. Every other request passes through,
// including no-cache, reload and only-if-cached requests (the background
// updater's manifest and HTML checks), and every URL the bundle lacks, so a
// new build still fills the HTTP cache from the network.

// True when `request` takes the cache-first path.
bool ShouldTryBootBundle(const BootBundleStore& store,
                         const network::ResourceRequest& request);

// Binds a proxying factory in front of `target`. `store` must outlive it.
void CreateBootBundleURLLoaderFactory(
    const BootBundleStore* store,
    mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver,
    mojo::PendingRemote<network::mojom::URLLoaderFactory> target);

// URLLoaderFactory interceptor hook: proxies only when a bundle is loaded.
void MaybeProxyURLLoaderFactoryForBootBundle(
    content::BrowserContext* browser_context,
    content::RenderFrameHost* frame,
    bool is_navigation,
    network::URLLoaderFactoryBuilder& builder);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_BOOT_BUNDLE_BOOT_BUNDLE_URL_LOADER_H_
