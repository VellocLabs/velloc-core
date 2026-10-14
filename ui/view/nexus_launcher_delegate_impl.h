// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_UI_VIEW_NEXUS_LAUNCHER_DELEGATE_IMPL_H_
#define CUSTOM_BROWSER_UI_VIEW_NEXUS_LAUNCHER_DELEGATE_IMPL_H_

namespace custom_browser {

// Installs the process-global nexus::NexusLauncherDelegate that opens/focuses
// the Nexus window when the launcher's tray icon is clicked. Idempotent; call
// once at startup.
//
// This lives in the UI layer (compiled into //chrome/browser/ui alongside
// custom_browser::window_util) because the launcher target cannot depend on
// //chrome/browser/ui without forming a build cycle — see
// custom_browser/browser/nexus/launcher/nexus_launcher_delegate.h.
void InstallNexusLauncherDelegate();

}  // namespace custom_browser

#endif  // CUSTOM_BROWSER_UI_VIEW_NEXUS_LAUNCHER_DELEGATE_IMPL_H_
