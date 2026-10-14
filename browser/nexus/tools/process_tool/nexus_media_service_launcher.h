// Copyright 2026 VellocLabs
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_PROCESS_TOOL_NEXUS_MEDIA_SERVICE_LAUNCHER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_PROCESS_TOOL_NEXUS_MEDIA_SERVICE_LAUNCHER_H_

#include "base/time/time.h"
#include "custom_browser/browser/nexus/mojom/nexus_media.mojom.h"
#include "custom_browser/browser/nexus/tools/process_tool/nexus_utility_service_remote.h"
#include "mojo/public/cpp/bindings/remote.h"

namespace nexus {

class NexusMediaServiceLauncher {
 public:
  NexusMediaServiceLauncher();
  NexusMediaServiceLauncher(const NexusMediaServiceLauncher&) = delete;
  NexusMediaServiceLauncher& operator=(const NexusMediaServiceLauncher&) = delete;
  ~NexusMediaServiceLauncher();

  mojo::Remote<mojom::NexusMediaService>& GetService();
  void Shutdown();

  static constexpr base::TimeDelta kIdleTimeout = base::Seconds(30);

 private:
  NexusUtilityServiceRemote<mojom::NexusMediaService> remote_;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_PROCESS_TOOL_NEXUS_MEDIA_SERVICE_LAUNCHER_H_