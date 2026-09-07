// Copyright 2026 The Custom Browser Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_FEATURES_NEXUS_KEYED_SERVICE_REGISTRATION_H_
#define CUSTOM_BROWSER_FEATURES_NEXUS_KEYED_SERVICE_REGISTRATION_H_

#include "custom_browser/chromium_hooks/hook_types.h"

namespace nexus {

// Constructs every Nexus BrowserContextKeyedServiceFactory, which is what
// registers it with the BrowserContextDependencyManager.
//
// The window for doing this is narrow at BOTH ends:
//
//  * Not before BrowserProcessImpl::Init(). A factory ctor may DependsOn a
//    Chrome factory — NexusBackgroundPanelHostFactory declares
//    ThemeServiceFactory — and ThemeServiceFactory reaches
//    ChromeExtensionSystemFactory, whose ctor DCHECKs on
//    `ExtensionsBrowserClient::Get()` ("must be initialized after
//    BrowserProcess"). That client is installed by BrowserProcessImpl::Init(),
//    which runs in ChromeBrowserMainParts::PreCreateThreadsImpl().
//  * Not after ChromeBrowserMainParts::PreProfileInit(), which calls
//    DependencyManager::DisallowKeyedServiceFactoryRegistration() — a factory
//    first reached lazily later (e.g. NexusSessionTabTracker from the bridge
//    adapter at GuestReady) DCHECK-fails there.
//
// PreCreateThreads on a ChromeBrowserMainExtraParts sits between the two, so
// that is where NexusKeyedServiceLifecycleHook calls this. Chrome registers
// its own factories the same way (glic among them), from
// ChromeBrowserMainExtraPartsProfiles.
//
// Idempotent — the factories are NoDestructor singletons.
void RegisterNexusKeyedServiceFactories();

// Runs RegisterNexusKeyedServiceFactories() at PreCreateThreads. Registered
// with the HookRegistrar from RegisterAllHooks(); constructing it must stay
// inert, because RegisterAllHooks() itself runs from
// CustomBrowserMainExtraParts' constructor — i.e. inside
// ChromeBrowserMainParts::Create(), where g_browser_process is still null.
class NexusKeyedServiceLifecycleHook
    : public custom_browser::BrowserMainLifecycleHook {
 public:
  NexusKeyedServiceLifecycleHook();
  ~NexusKeyedServiceLifecycleHook() override;

  // custom_browser::BrowserMainLifecycleHook:
  void PreCreateThreads() override;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_FEATURES_NEXUS_KEYED_SERVICE_REGISTRATION_H_
