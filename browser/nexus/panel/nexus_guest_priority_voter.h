// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_PANEL_NEXUS_GUEST_PRIORITY_VOTER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_PANEL_NEXUS_GUEST_PRIORITY_VOTER_H_

#include "base/callback_list.h"
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
// child frames included, so the runtime's, the UI's and the spawner frames'
// renderers stay out of the OS background class while hidden or parked. A
// frame `skip_frame` answers true for gets no vote, nor do the frames below
// it; Chromium's own votes place them. (P-2, F-42)

// Contract: on the performance-manager graph's sequence (UI thread);
// `is_guest_page` is asked on page add, on every main-frame URL change, and
// for every page each time `subscribe_to_changes` reports a change.
// `skip_frame` is asked when a frame would be voted for.
class NexusGuestPriorityVoter
    : public performance_manager::execution_context_priority::PriorityVoter,
      public performance_manager::PageNodeObserver,
      public performance_manager::FrameNodeObserver {
 public:
  using IsGuestPageCallback = base::RepeatingCallback<bool(
      const performance_manager::PageNode*)>;
  // Registers a closure to run when `is_guest_page`'s answer may have
  // changed for some page; the subscription ends the registration.
  using SubscribeCallback =
      base::RepeatingCallback<base::CallbackListSubscription(
          base::RepeatingClosure)>;

  using SkipFrameCallback = base::RepeatingCallback<bool(
      const performance_manager::FrameNode*)>;

  static const char kGuestPageReason[];

  explicit NexusGuestPriorityVoter(
      IsGuestPageCallback is_guest_page,
      SubscribeCallback subscribe_to_changes = SubscribeCallback(),
      SkipFrameCallback skip_frame = SkipFrameCallback());
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
  void OnMainFrameUrlChanged(const performance_manager::PageNode* page_node,
                             const GURL& previous_url) override;

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
  void UpdateAllPages();
  void UpdatePage(const performance_manager::PageNode* page_node);
  void VoteForSubtree(const performance_manager::FrameNode* frame_node);
  void InvalidateSubtree(const performance_manager::FrameNode* frame_node);
  void VoteForFrame(const performance_manager::FrameNode* frame_node);
  void InvalidateFrame(const performance_manager::FrameNode* frame_node);

  bool Skips(const performance_manager::FrameNode* frame_node) const;

  IsGuestPageCallback is_guest_page_;
  SubscribeCallback subscribe_to_changes_;
  SkipFrameCallback skip_frame_;
  base::CallbackListSubscription changes_subscription_;
  raw_ptr<performance_manager::Graph> graph_ = nullptr;
  performance_manager::execution_context_priority::VotingChannel
      voting_channel_;
  absl::flat_hash_set<raw_ptr<const performance_manager::PageNode>>
      guest_pages_;
  absl::flat_hash_set<raw_ptr<const performance_manager::FrameNode>>
      voted_frames_;
};

// The production predicate: the page's WebContents is a Nexus panel guest,
// and not a guest of a window's page parked while the profile's runtime host
// page is live.
bool IsNexusGuestPage(const performance_manager::PageNode* page_node);

// The production `skip_frame`: the frame is an app-page frame (F-42), which
// runs in a renderer of its own and follows the stock visibility votes.
bool IsAppPageFrameNode(const performance_manager::FrameNode* frame_node);

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
