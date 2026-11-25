// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_PANEL_NEXUS_PARKED_PAGE_WORKERS_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_PANEL_NEXUS_PARKED_PAGE_WORKERS_H_

#include <map>
#include <set>

#include "base/functional/callback.h"
#include "base/scoped_multi_source_observation.h"
#include "content/public/browser/dedicated_worker_service.h"
#include "content/public/browser/web_contents_observer.h"
#include "third_party/blink/public/common/tokens/tokens.h"

namespace nexus {

// The dedicated workers a parked page runs, by token. A worker created by a
// frame of the page's guests is the page's; so is a worker created by one of
// the page's workers, whichever of the two is reported first. UI thread.
class ParkedWorkerSet {
 public:
  ParkedWorkerSet();
  ~ParkedWorkerSet();
  ParkedWorkerSet(const ParkedWorkerSet&) = delete;
  ParkedWorkerSet& operator=(const ParkedWorkerSet&) = delete;

  // Each returns whether the set went from empty to not empty.
  bool AddFromPage(const blink::DedicatedWorkerToken& worker);
  bool AddNested(const blink::DedicatedWorkerToken& worker,
                 const blink::DedicatedWorkerToken& parent);
  // Returns whether the set went from not empty to empty.
  bool Remove(const blink::DedicatedWorkerToken& worker);
  bool empty() const { return workers_.empty(); }

 private:
  void Admit(const blink::DedicatedWorkerToken& worker);

  std::set<blink::DedicatedWorkerToken> workers_;
  // Workers whose creating worker is not in the set yet, by creator.
  std::multimap<blink::DedicatedWorkerToken, blink::DedicatedWorkerToken>
      orphans_;
};

// Follows the dedicated workers that frames of a parked page's guests run:
// run_js and mod workers spawned through that window's spawner frames, and
// the window's import worker. Guests attached after parking are followed
// too, with the workers they already run. `on_changed` runs when
// `has_workers()` changes, never from inside `Start()`. (P-2)
class NexusParkedPageWorkers
    : public content::WebContentsObserver,
      public content::DedicatedWorkerService::Observer {
 public:
  NexusParkedPageWorkers(content::WebContents* parked,
                         base::RepeatingClosure on_changed);
  ~NexusParkedPageWorkers() override;
  NexusParkedPageWorkers(const NexusParkedPageWorkers&) = delete;
  NexusParkedPageWorkers& operator=(const NexusParkedPageWorkers&) = delete;

  // Takes in the guests attached now and the workers they already run.
  void Start();

  bool has_workers() const { return !workers_.empty(); }

  // content::WebContentsObserver:
  void InnerWebContentsAttached(
      content::WebContents* inner_web_contents,
      content::RenderFrameHost* render_frame_host) override;

  // content::DedicatedWorkerService::Observer:
  void OnWorkerCreated(const blink::DedicatedWorkerToken& worker_token,
                       content::ChildProcessId worker_process_id,
                       const url::Origin& security_origin,
                       content::DedicatedWorkerCreator creator) override;
  void OnBeforeWorkerDestroyed(const blink::DedicatedWorkerToken& worker_token,
                               content::DedicatedWorkerCreator creator) override;
  void OnFinalResponseURLDetermined(
      const blink::DedicatedWorkerToken& worker_token,
      const GURL& url) override {}

 private:
  void ObserveGuest(content::WebContents* guest);
  bool IsFrameOfParkedPage(const content::GlobalRenderFrameHostId& id) const;

  base::RepeatingClosure on_changed_;
  ParkedWorkerSet workers_;
  bool enumerating_ = false;
  base::ScopedMultiSourceObservation<content::DedicatedWorkerService,
                                     content::DedicatedWorkerService::Observer>
      observations_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_PANEL_NEXUS_PARKED_PAGE_WORKERS_H_
