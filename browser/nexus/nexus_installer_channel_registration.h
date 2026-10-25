// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_INSTALLER_CHANNEL_REGISTRATION_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_INSTALLER_CHANNEL_REGISTRATION_H_

namespace custom_browser {
class HookRegistrar;
}  // namespace custom_browser

namespace nexus {

// Registers the startup read of the installer's channel marker in the user
// data dir (G-3), at PreCreateThreads.
void RegisterInstallerChannelHooks(custom_browser::HookRegistrar* registrar);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_NEXUS_INSTALLER_CHANNEL_REGISTRATION_H_
