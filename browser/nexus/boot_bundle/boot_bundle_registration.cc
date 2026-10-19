// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/boot_bundle/boot_bundle_registration.h"

#include <memory>

#include "base/base_paths.h"
#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/path_service.h"
#include "custom_browser/browser/nexus/boot_bundle/boot_bundle_store.h"
#include "custom_browser/browser/nexus/boot_bundle/boot_bundle_url_loader.h"
#include "custom_browser/chromium_hooks/hook_registrar.h"
#include "custom_browser/chromium_hooks/hook_types.h"

namespace nexus {

namespace {

class BootBundleLifecycleHook : public custom_browser::BrowserMainLifecycleHook {
 public:
  void PostCreateThreads() override {
    base::FilePath module_dir;
    if (!base::PathService::Get(base::DIR_MODULE, &module_dir)) {
      return;
    }
    store_ = std::make_unique<BootBundleStore>();
    BootBundleStore::SetGlobal(store_.get());
    store_->Load(module_dir);
  }

  void PostMainMessageLoopRun() override {
    BootBundleStore::SetGlobal(nullptr);
  }

 private:
  // Kept until process exit: a proxying factory may still hold a pointer.
  std::unique_ptr<BootBundleStore> store_;
};

}  // namespace

void RegisterBootBundleHooks(custom_browser::HookRegistrar* registrar) {
  registrar->AddBrowserMainLifecycleHook(
      std::make_unique<BootBundleLifecycleHook>());
  registrar->AddURLLoaderFactoryInterceptor(
      base::BindRepeating(&MaybeProxyURLLoaderFactoryForBootBundle));
}

}  // namespace nexus
