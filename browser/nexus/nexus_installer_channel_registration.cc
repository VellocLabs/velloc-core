// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/nexus_installer_channel_registration.h"

#include <memory>

#include "base/files/file_path.h"
#include "base/path_service.h"
#include "chrome/common/chrome_paths.h"
#include "custom_browser/browser/nexus/nexus_installer_channel.h"
#include "custom_browser/chromium_hooks/hook_registrar.h"
#include "custom_browser/chromium_hooks/hook_types.h"

namespace nexus {

namespace {

class InstallerChannelLifecycleHook
    : public custom_browser::BrowserMainLifecycleHook {
 public:
  void PreCreateThreads() override {
    base::FilePath user_data_dir;
    if (base::PathService::Get(chrome::DIR_USER_DATA, &user_data_dir)) {
      LoadPendingInstallerChannel(user_data_dir);
    }
  }
};

}  // namespace

void RegisterInstallerChannelHooks(custom_browser::HookRegistrar* registrar) {
  registrar->AddBrowserMainLifecycleHook(
      std::make_unique<InstallerChannelLifecycleHook>());
}

}  // namespace nexus
