// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_FEATURES_NEXUS_KEYED_SERVICE_REGISTRATION_H_
#define CUSTOM_BROWSER_FEATURES_NEXUS_KEYED_SERVICE_REGISTRATION_H_

#include "custom_browser/chromium_hooks/hook_types.h"

namespace nexus {

// Constructs, and so registers, every Nexus BrowserContextKeyedServiceFactory.
// Must run after BrowserProcessImpl::Init() (factory dependencies DCHECK before
// it) and before PreProfileInit() disallows registration: at PreCreateThreads.
// Idempotent.
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
