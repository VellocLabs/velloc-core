// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/app_info/nexus_render_cache_purger.h"

#include <array>
#include <memory>
#include <set>

#include "base/check.h"
#include "base/no_destructor.h"
#include "base/supports_user_data.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/memory_pressure_for_embedder.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/web_contents.h"
#include "custom_browser/browser/nexus/panel/nexus_panel_guest.h"
#include "custom_browser/browser/nexus/session/nexus_session_tab_tag.h"

namespace nexus {
namespace {

constexpr std::array<NexusRenderCachePurger::Target, 3> kTargets = {
    NexusRenderCachePurger::kGuest, NexusRenderCachePurger::kGpu,
    NexusRenderCachePurger::kAgentTabs};

using SendTimes = std::array<base::TimeTicks, kTargets.size()>;

// The GPU process is shared by every profile; renderer targets are not.
struct PurgerState {
  NexusRenderCachePurger::RendererSender renderer_for_testing;
  NexusRenderCachePurger::GpuSender gpu_for_testing;
  base::TimeTicks gpu_last_sent;
  // Renderer stamps when no profile is given; never set, since nothing is sent.
  SendTimes no_profile_last_sent;
  // Bumped by SetSendersForTesting so per-profile stamps from an earlier test
  // read as unset.
  int generation = 0;
};

struct ProfileSendTimes : public base::SupportsUserData::Data {
  SendTimes last_sent;
  int generation = 0;
};

const char kProfileSendTimesKey[] = "nexus.render_cache_purger";

PurgerState& State() {
  static base::NoDestructor<PurgerState> state;
  return *state;
}

size_t IndexOf(NexusRenderCachePurger::Target target) {
  for (size_t i = 0; i < kTargets.size(); ++i) {
    if (kTargets[i] == target) {
      return i;
    }
  }
  NOTREACHED();
}

// Last-send stamps for `target`: the shared GPU stamp, or `profile`'s own.
base::TimeTicks& LastSent(Profile* profile,
                          NexusRenderCachePurger::Target target) {
  if (target == NexusRenderCachePurger::kGpu) {
    return State().gpu_last_sent;
  }
  if (!profile) {
    return State().no_profile_last_sent[IndexOf(target)];
  }
  auto* times = static_cast<ProfileSendTimes*>(
      profile->GetUserData(kProfileSendTimesKey));
  if (!times) {
    auto owned = std::make_unique<ProfileSendTimes>();
    times = owned.get();
    profile->SetUserData(kProfileSendTimesKey, std::move(owned));
  }
  if (times->generation != State().generation) {
    times->last_sent = {};
    times->generation = State().generation;
  }
  return times->last_sent[IndexOf(target)];
}

void SendOneToRenderer(content::RenderProcessHost* host,
                       base::MemoryPressureLevel level) {
  if (State().renderer_for_testing) {
    State().renderer_for_testing.Run(host, level);
    return;
  }
  content::NotifyRendererMemoryPressure(host, level);
}

int SendOneToGpu(base::MemoryPressureLevel level) {
  if (State().gpu_for_testing) {
    return State().gpu_for_testing.Run(level);
  }
  return content::NotifyGpuMemoryPressure(level);
}

// A child process keeps the last level it was sent until the browser sends
// another, and a held level caps its caches and makes a repeat of the same
// level a no-op. Each purge therefore sends `level` and then NONE.
void SendToRenderer(content::RenderProcessHost* host,
                    base::MemoryPressureLevel level) {
  SendOneToRenderer(host, level);
  if (level != base::MEMORY_PRESSURE_LEVEL_NONE) {
    SendOneToRenderer(host, base::MEMORY_PRESSURE_LEVEL_NONE);
  }
}

int SendToGpu(base::MemoryPressureLevel level) {
  const int notified = SendOneToGpu(level);
  if (notified && level != base::MEMORY_PRESSURE_LEVEL_NONE) {
    SendOneToGpu(base::MEMORY_PRESSURE_LEVEL_NONE);
  }
  return notified;
}

// Which of `kGuest` / `kAgentTabs` the frames of `host` belong to.
uint32_t ClassifyProcess(content::RenderProcessHost* host) {
  uint32_t kinds = 0;
  host->ForEachRenderFrameHost([&kinds](content::RenderFrameHost* rfh) {
    content::WebContents* contents =
        content::WebContents::FromRenderFrameHost(rfh);
    if (!contents) {
      return;
    }
    if (IsNexusPanelGuestContents(contents)) {
      kinds |= NexusRenderCachePurger::kGuest;
    } else if (NexusSessionTabTag::SessionKeyFor(contents)) {
      kinds |= NexusRenderCachePurger::kAgentTabs;
    }
  });
  return kinds;
}

}  // namespace

// static
NexusRenderCachePurger::Result NexusRenderCachePurger::Purge(
    Profile* profile,
    uint32_t targets,
    base::MemoryPressureLevel level) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  Result result;
  const base::TimeTicks now = base::TimeTicks::Now();
  for (Target target : kTargets) {
    if (!(targets & target)) {
      continue;
    }
    const base::TimeTicks last = LastSent(profile, target);
    if (!last.is_null() && now - last < kMinInterval) {
      result.throttled |= target;
      continue;
    }
    result.purged |= target;
  }

  const uint32_t renderer_targets = result.purged & (kGuest | kAgentTabs);
  if (renderer_targets && profile) {
    std::set<content::RenderProcessHost*> sent;
    for (auto it = content::RenderProcessHost::AllHostsIterator();
         !it.IsAtEnd(); it.Advance()) {
      content::RenderProcessHost* host = it.GetCurrentValue();
      if (host->GetBrowserContext() != profile) {
        continue;
      }
      const uint32_t kinds = ClassifyProcess(host) & renderer_targets;
      if (!kinds) {
        continue;
      }
      if (kinds & kGuest) {
        ++result.guest_processes;
      }
      if (kinds & kAgentTabs) {
        ++result.agent_tab_processes;
      }
      if (sent.insert(host).second) {
        SendToRenderer(host, level);
      }
    }
  }
  if (result.purged & kGpu) {
    result.gpu_processes = SendToGpu(level);
  }

  // Only a target that reached a process starts its interval.
  if (result.guest_processes) {
    LastSent(profile, kGuest) = now;
  }
  if (result.agent_tab_processes) {
    LastSent(profile, kAgentTabs) = now;
  }
  if (result.gpu_processes) {
    LastSent(profile, kGpu) = now;
  }
  return result;
}

// static
void NexusRenderCachePurger::SetSendersForTesting(RendererSender renderer,
                                                  GpuSender gpu) {
  State().renderer_for_testing = std::move(renderer);
  State().gpu_for_testing = std::move(gpu);
  State().gpu_last_sent = base::TimeTicks();
  ++State().generation;
}

}  // namespace nexus
