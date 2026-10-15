// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_PANEL_NEXUS_GUEST_PRIORITY_VOTER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_PANEL_NEXUS_GUEST_PRIORITY_VOTER_H_

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "components/performance_manager/public/execution_context_priority/execution_context_priority.h"
#include "components/performance_manager/public/execution_context_priority/priority_voting_system.h"
#include "components/performance_manager/public/graph/frame_node.h"
#include "components/performance_manager/public/graph/page_node.h"
#include "custom_browser/chromium_hooks/hook_types.h"
#include "third_party/abseil-cpp/absl/container/flat_hash_set.h"

namespace nexus {

// Casts a USER_BLOCKING priority vote for every frame of a Nexus guest page,
// sandboxed child frames included, so the guest's renderers (the UI with the
// runtime SharedWorker, and the mod-host frames' own renderer) stay out of
// the OS background class while the guest is hidden or parked. (P-2)
//
// Contract: on the performance-manager graph's sequence (UI thread);
// `is_guest_page` is asked on page add and on every main-frame URL change.
class NexusGuestPriorityVoter
    : public performance_manager::execution_context_priority::PriorityVoter,
      public performance_manager::PageNodeObserver,
      public performance_manager::FrameNodeObserver {
 public:
  using IsGuestPageCallback = base::RepeatingCallback<bool(
      const performance_manager::PageNode*)>;

  static const char kGuestPageReason[];

  explicit NexusGuestPriorityVoter(IsGuestPageCallback is_guest_page);
  ~NexusGuestPriorityVoter() override;

  NexusGuestPriorityVoter(const NexusGuestPriorityVoter&) = delete;
  NexusGuestPriorityVoter& operator=(const NexusGuestPriorityVoter&) = delete;

  // PriorityVoter:
  void InitializeOnGraph(
      performance_manager::Graph* graph,
      performance_manager::execution_context_priority::VotingChannel
          voting_channel) override;
  void TearDownOnGraph(performance_manager::Graph* graph) override;

  // PageNodeObserver:
  void OnPageNodeAdded(const performance_manager::PageNode* page_node) override;
  void OnBeforePageNodeRemoved(
      const performance_manager::PageNode* page_node) override;
  void OnMainFrameUrlChanged(
      const performance_manager::PageNode* page_node) override;

  // FrameNodeObserver:
  void OnBeforeFrameNodeAdded(
      const performance_manager::FrameNode* frame_node,
      const performance_manager::FrameNode* pending_parent_frame_node,
      const performance_manager::PageNode* pending_page_node,
      const performance_manager::ProcessNode* pending_process_node,
      const performance_manager::FrameNode*
          pending_parent_or_outer_document_or_embedder) override;
  void OnBeforeFrameNodeRemoved(
      const performance_manager::FrameNode* frame_node) override;

  performance_manager::execution_context_priority::VoterId voter_id() const {
    return voting_channel_.voter_id();
  }

 private:
  void UpdatePage(const performance_manager::PageNode* page_node);
  void VoteForSubtree(const performance_manager::FrameNode* frame_node);
  void InvalidateSubtree(const performance_manager::FrameNode* frame_node);
  void VoteForFrame(const performance_manager::FrameNode* frame_node);
  void InvalidateFrame(const performance_manager::FrameNode* frame_node);

  IsGuestPageCallback is_guest_page_;
  performance_manager::execution_context_priority::VotingChannel
      voting_channel_;
  absl::flat_hash_set<raw_ptr<const performance_manager::PageNode>>
      guest_pages_;
  absl::flat_hash_set<raw_ptr<const performance_manager::FrameNode>>
      voted_frames_;
};

// The production predicate: the page's WebContents is the Nexus panel guest.
bool IsNexusGuestPage(const performance_manager::PageNode* page_node);

// Adds a NexusGuestPriorityVoter to the performance manager's voting system.
// No-op when the performance manager or its voting system is absent.
void InstallNexusGuestPriorityVoter();

// Installs the voter at PreMainMessageLoopRun, after the performance manager's
// graph exists.
class NexusGuestPriorityLifecycleHook
    : public custom_browser::BrowserMainLifecycleHook {
 public:
  // custom_browser::BrowserMainLifecycleHook:
  void PreMainMessageLoopRun() override;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_PANEL_NEXUS_GUEST_PRIORITY_VOTER_H_
