// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/utility/nexus/shell/services.h"

#include <memory>
#include <utility>

#include "custom_browser/browser/nexus/mojom/nexus_media.mojom.h"
#include "custom_browser/browser/nexus/mojom/nexus_search.mojom.h"
#include "custom_browser/browser/nexus/mojom/nexus_shell.mojom.h"
#include "custom_browser/utility/nexus/media/nexus_media_service_impl.h"
#include "custom_browser/utility/nexus/search/nexus_search_service_impl.h"
#include "custom_browser/utility/nexus/shell/nexus_shell_service_impl.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"

namespace nexus {

namespace {

// Factory invoked by the Mojo service runner when the browser launches a
// new utility process for `nexus.mojom.NexusShellService`. Mirrors the
// `auto RunFoo(...)` pattern in chrome/utility/services.cc so the upstream
// registration call site can stay symmetric with the other services.
std::unique_ptr<NexusShellServiceImpl> RunNexusShellService(
    mojo::PendingReceiver<mojom::NexusShellService> receiver) {
  return std::make_unique<NexusShellServiceImpl>(std::move(receiver));
}

std::unique_ptr<NexusMediaServiceImpl> RunNexusMediaService(
    mojo::PendingReceiver<mojom::NexusMediaService> receiver) {
  return std::make_unique<NexusMediaServiceImpl>(std::move(receiver));
}

std::unique_ptr<NexusSearchServiceImpl> RunNexusSearchService(
    mojo::PendingReceiver<mojom::NexusSearchService> receiver) {
  return std::make_unique<NexusSearchServiceImpl>(std::move(receiver));
}

}  // namespace

void RegisterNexusUtilityServices(mojo::ServiceFactory& services) {
  services.Add(RunNexusShellService);
  services.Add(RunNexusMediaService);
  services.Add(RunNexusSearchService);
}

}  // namespace nexus
