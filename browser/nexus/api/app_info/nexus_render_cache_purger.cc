// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/app_info/nexus_render_cache_purger.h"

#include <array>
#include <set>

#include "base/check.h"
#include "base/no_destructor.h"
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

struct PurgerState {
  NexusRenderCachePurger::RendererSender renderer_for_testing;
  NexusRenderCachePurger::GpuSender gpu_for_testing;
  std::array<base::TimeTicks, kTargets.size()> last_sent;
};

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

void SendToRenderer(content::RenderProcessHost* host,
                    base::MemoryPressureLevel level) {
  if (State().renderer_for_testing) {
    State().renderer_for_testing.Run(host, level);
    return;
  }
  content::NotifyRendererMemoryPressure(host, level);
}

int SendToGpu(base::MemoryPressureLevel level) {
  if (State().gpu_for_testing) {
    return State().gpu_for_testing.Run(level);
  }
  return content::NotifyGpuMemoryPressure(level);
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
    base::TimeTicks& last = State().last_sent[IndexOf(target)];
    if (!last.is_null() && now - last < kMinInterval) {
      result.throttled |= target;
      continue;
    }
    last = now;
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
  return result;
}

// static
void NexusRenderCachePurger::SetSendersForTesting(RendererSender renderer,
                                                  GpuSender gpu) {
  State().renderer_for_testing = std::move(renderer);
  State().gpu_for_testing = std::move(gpu);
  State().last_sent = {};
}

}  // namespace nexus
