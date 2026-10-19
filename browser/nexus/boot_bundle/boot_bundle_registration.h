// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_BOOT_BUNDLE_BOOT_BUNDLE_REGISTRATION_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_BOOT_BUNDLE_BOOT_BUNDLE_REGISTRATION_H_

namespace custom_browser {
class HookRegistrar;
}  // namespace custom_browser

namespace nexus {

// Registers the guest boot bundle (F-33): loads the bundle next to
// chrome.dll after thread creation, and proxies URLLoaderFactories.
void RegisterBootBundleHooks(custom_browser::HookRegistrar* registrar);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_BOOT_BUNDLE_BOOT_BUNDLE_REGISTRATION_H_
