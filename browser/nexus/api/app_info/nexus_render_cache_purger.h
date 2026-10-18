// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_APP_INFO_NEXUS_RENDER_CACHE_PURGER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_APP_INFO_NEXUS_RENDER_CACHE_PURGER_H_

#include <cstdint>

#include "base/functional/callback.h"
#include "base/memory/memory_pressure_listener.h"
#include "base/time/time.h"

class Profile;

namespace content {
class RenderProcessHost;
}

namespace nexus {

// Contract: asks the renderers and the GPU process a Nexus client uses to drop
// their caches by sending them a memory-pressure level directly, without the
// browser-global signal that would discard tabs. Nothing is terminated or
// frozen: pages, SharedWorkers and dedicated workers keep running (P-3).
// UI thread only. Rate-limited per target: per profile for the
// renderer targets, process-wide for the GPU. A target that reached a process
// less than `kMinInterval` ago is reported as throttled, not sent; a purge that
// reached no process does not start the interval.
class NexusRenderCachePurger {
 public:
  enum Target : uint32_t {
    // Every renderer hosting a frame of a Nexus panel guest (all windows of
    // the profile, including the mod-host frames' own renderer).
    kGuest = 1u << 0,
    // The GPU process.
    kGpu = 1u << 1,
    // Every renderer hosting a frame of a tab tagged with a Nexus session.
    kAgentTabs = 1u << 2,
  };

  struct Result {
    uint32_t purged = 0;
    uint32_t throttled = 0;
    int guest_processes = 0;
    int agent_tab_processes = 0;
    int gpu_processes = 0;
  };

  using RendererSender =
      base::RepeatingCallback<void(content::RenderProcessHost*,
                                   base::MemoryPressureLevel)>;
  // Returns how many GPU processes were notified.
  using GpuSender = base::RepeatingCallback<int(base::MemoryPressureLevel)>;

  static constexpr base::TimeDelta kMinInterval = base::Seconds(10);

  NexusRenderCachePurger() = delete;

  // Sends `level` and then NONE, so no process keeps a pressure level, to each
  // target in the `targets` bitmask that is not throttled; each renderer
  // process at most once per call. On Windows and macOS the GPU process frees
  // only its shader and program caches and discardable memory (P-3).
  static Result Purge(Profile* profile,
                      uint32_t targets,
                      base::MemoryPressureLevel level);

  // Replaces the content senders and clears the rate limit; null callbacks
  // restore the real senders.
  static void SetSendersForTesting(RendererSender renderer, GpuSender gpu);
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_APP_INFO_NEXUS_RENDER_CACHE_PURGER_H_
