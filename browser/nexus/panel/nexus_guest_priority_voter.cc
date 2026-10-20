// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/panel/nexus_guest_priority_voter.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/process/process.h"
#include "components/performance_manager/public/execution_context/execution_context.h"
#include "components/performance_manager/public/graph/graph.h"
#include "components/performance_manager/public/performance_manager.h"
#include "content/public/browser/web_contents.h"
#include "custom_browser/browser/nexus/panel/nexus_panel_guest.h"

namespace nexus {

namespace {

using performance_manager::FrameNode;
using performance_manager::Graph;
using performance_manager::PageNode;
using performance_manager::execution_context::ExecutionContext;
using performance_manager::execution_context_priority::PriorityVotingSystem;
using performance_manager::execution_context_priority::Vote;
using performance_manager::execution_context_priority::VotingChannel;

}  // namespace

// static
const char NexusGuestPriorityVoter::kGuestPageReason[] = "Nexus guest page.";

NexusGuestPriorityVoter::NexusGuestPriorityVoter(
    IsGuestPageCallback is_guest_page)
    : is_guest_page_(std::move(is_guest_page)) {}

NexusGuestPriorityVoter::~NexusGuestPriorityVoter() = default;

void NexusGuestPriorityVoter::InitializeOnGraph(Graph* graph,
                                                VotingChannel voting_channel) {
  voting_channel_ = std::move(voting_channel);
  graph->AddPageNodeObserver(this);
  graph->AddFrameNodeObserver(this);
  for (const PageNode* page_node : graph->GetAllPageNodes()) {
    UpdatePage(page_node);
  }
}

void NexusGuestPriorityVoter::TearDownOnGraph(Graph* graph) {
  graph->RemoveFrameNodeObserver(this);
  graph->RemovePageNodeObserver(this);
  for (const FrameNode* frame_node : voted_frames_) {
    voting_channel_.InvalidateVote(ExecutionContext::From(frame_node));
  }
  voted_frames_.clear();
  guest_pages_.clear();
  voting_channel_.Reset();
}

void NexusGuestPriorityVoter::OnPageNodeAdded(const PageNode* page_node) {
  UpdatePage(page_node);
}

void NexusGuestPriorityVoter::OnBeforePageNodeRemoved(
    const PageNode* page_node) {
  // Its frames are gone by now, and their votes with them.
  guest_pages_.erase(page_node);
}

void NexusGuestPriorityVoter::OnMainFrameUrlChanged(const PageNode* page_node,
                                                    const GURL& previous_url) {
  UpdatePage(page_node);
}

void NexusGuestPriorityVoter::OnBeforeFrameNodeAdded(
    const FrameNode* frame_node,
    const FrameNode* pending_parent_frame_node,
    const PageNode* pending_page_node,
    const performance_manager::ProcessNode* pending_process_node,
    const FrameNode* pending_parent_or_outer_document_or_embedder) {
  if (guest_pages_.contains(pending_page_node)) {
    VoteForFrame(frame_node);
  }
}

void NexusGuestPriorityVoter::OnBeforeFrameNodeRemoved(
    const FrameNode* frame_node) {
  InvalidateFrame(frame_node);
}

void NexusGuestPriorityVoter::UpdatePage(const PageNode* page_node) {
  const bool is_guest = is_guest_page_.Run(page_node);
  if (is_guest == guest_pages_.contains(page_node)) {
    return;
  }
  if (is_guest) {
    guest_pages_.insert(page_node);
    for (const FrameNode* main_frame_node : page_node->GetMainFrameNodes()) {
      VoteForSubtree(main_frame_node);
    }
  } else {
    guest_pages_.erase(page_node);
    for (const FrameNode* main_frame_node : page_node->GetMainFrameNodes()) {
      InvalidateSubtree(main_frame_node);
    }
  }
}

void NexusGuestPriorityVoter::VoteForSubtree(const FrameNode* frame_node) {
  VoteForFrame(frame_node);
  for (const FrameNode* child_frame_node : frame_node->GetChildFrameNodes()) {
    VoteForSubtree(child_frame_node);
  }
}

void NexusGuestPriorityVoter::InvalidateSubtree(const FrameNode* frame_node) {
  InvalidateFrame(frame_node);
  for (const FrameNode* child_frame_node : frame_node->GetChildFrameNodes()) {
    InvalidateSubtree(child_frame_node);
  }
}

void NexusGuestPriorityVoter::VoteForFrame(const FrameNode* frame_node) {
  if (!voted_frames_.insert(frame_node).second) {
    return;
  }
  voting_channel_.SubmitVote(
      ExecutionContext::From(frame_node),
      Vote(base::Process::Priority::kUserBlocking, kGuestPageReason));
}

void NexusGuestPriorityVoter::InvalidateFrame(const FrameNode* frame_node) {
  if (voted_frames_.erase(frame_node) == 0) {
    return;
  }
  voting_channel_.InvalidateVote(ExecutionContext::From(frame_node));
}

bool IsNexusGuestPage(const PageNode* page_node) {
  base::WeakPtr<content::WebContents> contents = page_node->GetWebContents();
  return contents && IsNexusPanelGuestContents(contents.get());
}

void InstallNexusGuestPriorityVoter() {
  if (!performance_manager::PerformanceManager::IsAvailable()) {
    return;
  }
  auto* voting_system = PriorityVotingSystem::GetFromGraph(
      performance_manager::PerformanceManager::GetGraph());
  if (!voting_system) {
    return;
  }
  voting_system->AddPriorityVoter<NexusGuestPriorityVoter>(
      base::BindRepeating(&IsNexusGuestPage));
}

void NexusGuestPriorityLifecycleHook::PreMainMessageLoopRun() {
  InstallNexusGuestPriorityVoter();
}

}  // namespace nexus
