// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/tools/process_tool/nexus_media_service_launcher.h"

namespace nexus {

NexusMediaServiceLauncher::NexusMediaServiceLauncher()
    : remote_("Nexus Media Service", "NexusMediaServiceLauncher",
              kIdleTimeout) {}

NexusMediaServiceLauncher::~NexusMediaServiceLauncher() = default;

mojo::Remote<mojom::NexusMediaService>& NexusMediaServiceLauncher::GetService() {
  return remote_.GetService();
}

void NexusMediaServiceLauncher::Shutdown() {
  remote_.Shutdown();
}

}  // namespace nexus