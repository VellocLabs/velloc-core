// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/panel/nexus_parked_page_workers.h"

#include <utility>
#include <variant>
#include <vector>

#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"

namespace nexus {

ParkedWorkerSet::ParkedWorkerSet() = default;
ParkedWorkerSet::~ParkedWorkerSet() = default;

bool ParkedWorkerSet::AddFromPage(const blink::DedicatedWorkerToken& worker) {
  const bool was_empty = workers_.empty();
  Admit(worker);
  return was_empty && !workers_.empty();
}

bool ParkedWorkerSet::AddNested(const blink::DedicatedWorkerToken& worker,
                                const blink::DedicatedWorkerToken& parent) {
  if (!workers_.contains(parent)) {
    orphans_.emplace(parent, worker);
    return false;
  }
  return AddFromPage(worker);
}

bool ParkedWorkerSet::Remove(const blink::DedicatedWorkerToken& worker) {
  std::erase_if(orphans_, [&](const auto& entry) {
    return entry.second == worker;
  });
  return workers_.erase(worker) && workers_.empty();
}

void ParkedWorkerSet::Admit(const blink::DedicatedWorkerToken& worker) {
  std::vector<blink::DedicatedWorkerToken> pending = {worker};
  while (!pending.empty()) {
    const blink::DedicatedWorkerToken next = pending.back();
    pending.pop_back();
    if (!workers_.insert(next).second) {
      continue;
    }
    auto [begin, end] = orphans_.equal_range(next);
    for (auto it = begin; it != end; ++it) {
      pending.push_back(it->second);
    }
    orphans_.erase(begin, end);
  }
}

NexusParkedPageWorkers::NexusParkedPageWorkers(
    content::WebContents* parked,
    base::RepeatingClosure on_changed)
    : content::WebContentsObserver(parked),
      on_changed_(std::move(on_changed)) {}

NexusParkedPageWorkers::~NexusParkedPageWorkers() = default;

void NexusParkedPageWorkers::Start() {
  for (content::WebContents* guest : web_contents()->GetInnerWebContents()) {
    ObserveGuest(guest);
  }
}

void NexusParkedPageWorkers::InnerWebContentsAttached(
    content::WebContents* inner_web_contents,
    content::RenderFrameHost* render_frame_host) {
  const bool had_workers = has_workers();
  ObserveGuest(inner_web_contents);
  if (had_workers != has_workers()) {
    on_changed_.Run();
  }
}

void NexusParkedPageWorkers::ObserveGuest(content::WebContents* guest) {
  content::DedicatedWorkerService* service =
      guest->GetPrimaryMainFrame()
          ->GetStoragePartition()
          ->GetDedicatedWorkerService();
  if (observations_.IsObservingSource(service)) {
    return;
  }
  observations_.AddObservation(service);
  // Workers the guest started before it was observed.
  enumerating_ = true;
  service->EnumerateDedicatedWorkers(this);
  enumerating_ = false;
}

void NexusParkedPageWorkers::OnWorkerCreated(
    const blink::DedicatedWorkerToken& worker_token,
    content::ChildProcessId worker_process_id,
    const url::Origin& security_origin,
    content::DedicatedWorkerCreator creator) {
  bool became_busy = false;
  if (const auto* parent = std::get_if<blink::DedicatedWorkerToken>(&creator)) {
    became_busy = workers_.AddNested(worker_token, *parent);
  } else if (IsFrameOfParkedPage(
                 std::get<content::GlobalRenderFrameHostId>(creator))) {
    became_busy = workers_.AddFromPage(worker_token);
  }
  if (became_busy && !enumerating_) {
    on_changed_.Run();
  }
}

void NexusParkedPageWorkers::OnBeforeWorkerDestroyed(
    const blink::DedicatedWorkerToken& worker_token,
    content::DedicatedWorkerCreator creator) {
  if (workers_.Remove(worker_token)) {
    on_changed_.Run();
  }
}

bool NexusParkedPageWorkers::IsFrameOfParkedPage(
    const content::GlobalRenderFrameHostId& id) const {
  content::RenderFrameHost* frame = content::RenderFrameHost::FromID(id);
  content::WebContents* contents =
      frame ? content::WebContents::FromRenderFrameHost(frame) : nullptr;
  return contents && contents->GetOuterWebContents() == web_contents();
}

}  // namespace nexus
