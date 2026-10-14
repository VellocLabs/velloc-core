// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_PROCESS_TOOL_NEXUS_SEARCH_SERVICE_LAUNCHER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_PROCESS_TOOL_NEXUS_SEARCH_SERVICE_LAUNCHER_H_

#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "custom_browser/browser/nexus/mojom/nexus_search.mojom.h"
#include "custom_browser/browser/nexus/tools/process_tool/nexus_utility_service_remote.h"
#include "mojo/public/cpp/bindings/remote.h"

class Profile;

namespace nexus {

// Browser-side handle on the transcript-search utility process.
//
// Lives next to the shell and media launchers because it is the same
// pattern, not because it is a process tool: `NexusUtilityServiceRemote`
// owns the launch/disconnect/idle-recycle plumbing and this adds only the
// naming and the idle window.
class NexusSearchServiceLauncher {
 public:
  NexusSearchServiceLauncher();
  NexusSearchServiceLauncher(const NexusSearchServiceLauncher&) = delete;
  NexusSearchServiceLauncher& operator=(const NexusSearchServiceLauncher&) =
      delete;
  ~NexusSearchServiceLauncher();

  mojo::Remote<mojom::NexusSearchService>& GetService();
  void Shutdown();

  base::WeakPtr<NexusSearchServiceLauncher> GetWeakPtr();

  // Longer than the media service's 30s. The process holds the warm span
  // cache, so recycling it costs every subsequent search a re-read of the
  // `.sdi` sidecars. Five minutes keeps a session's worth of searching on
  // one warm process while still returning the memory to a user who has
  // moved on.
  static constexpr base::TimeDelta kIdleTimeout = base::Minutes(5);

 private:
  NexusUtilityServiceRemote<mojom::NexusSearchService> remote_;
  base::WeakPtrFactory<NexusSearchServiceLauncher> weak_factory_{this};
};

// One launcher — and therefore one utility process and one warm span
// cache — per profile, shared by every bridge port. Two guest panels
// searching the same transcripts reuse each other's index instead of
// paying for it twice.
NexusSearchServiceLauncher& GetSharedSearchLauncher(Profile* profile);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_PROCESS_TOOL_NEXUS_SEARCH_SERVICE_LAUNCHER_H_
