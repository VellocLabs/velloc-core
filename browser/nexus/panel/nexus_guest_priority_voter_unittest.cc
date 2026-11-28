// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/panel/nexus_guest_priority_voter.h"

#include <memory>
#include <optional>
#include <set>

#include "base/callback_list.h"
#include "base/functional/bind.h"
#include "base/time/time.h"
#include "components/performance_manager/graph/frame_node_impl.h"
#include "components/performance_manager/graph/page_node_impl.h"
#include "components/performance_manager/public/execution_context/execution_context.h"
#include "components/performance_manager/test_support/graph_test_harness.h"
#include "components/performance_manager/test_support/mock_graphs.h"
#include "components/performance_manager/test_support/voting.h"
#include "content/public/browser/browsing_instance_id.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace nexus {

namespace {

using performance_manager::FrameNode;
using performance_manager::FrameNodeImpl;
using performance_manager::GraphTestHarness;
using performance_manager::MockSinglePageInSingleProcessGraph;
using performance_manager::MockSinglePageWithMultipleProcessesGraph;
using performance_manager::PageNode;
using performance_manager::PageNodeImpl;
using performance_manager::TestNodeWrapper;
using performance_manager::execution_context::ExecutionContext;
using performance_manager::execution_context_priority::Vote;
using DummyVoteObserver =
    performance_manager::voting::test::DummyVoteObserver<Vote>;

// Test stand-in for "is the Nexus panel guest": keyed on the main-frame URL,
// so a main-frame commit is what makes a page a guest page.
constexpr char kGuestUrl[] = "https://guest.test/";
constexpr char kOtherUrl[] = "https://other.test/";

// Frames created in this browsing instance are skipped from the moment they
// are added, before a test can name them.
const content::BrowsingInstanceId kSkippedBrowsingInstance =
    content::BrowsingInstanceId::FromUnsafeValue(4242);

bool IsGuestUrl(const PageNode* page_node) {
  return page_node->GetMainFrameUrl() == GURL(kGuestUrl);
}

// A guest page whose answer can be switched off, as a parked window's page is
// when no session needs it.
bool g_guest_needed = true;

bool IsNeededGuestUrl(const PageNode* page_node) {
  return g_guest_needed && IsGuestUrl(page_node);
}

void Commit(PageNodeImpl* page, const char* url) {
  static int64_t navigation_id = 0;
  page->OnMainFrameNavigationCommitted(
      /*same_document=*/false, base::TimeTicks::Now(), ++navigation_id,
      GURL(url), "text/html", /*notification_permission_status=*/std::nullopt);
}

class NexusGuestPriorityVoterTest : public GraphTestHarness {
 protected:
  void TearDown() override {
    if (voter_) {
      voter_->TearDownOnGraph(graph());
      voter_.reset();
    }
    GraphTestHarness::TearDown();
  }

  void InstallVoter() {
    voter_ = std::make_unique<NexusGuestPriorityVoter>(
        base::BindRepeating(&IsGuestUrl));
    voter_->InitializeOnGraph(graph(), observer_.BuildVotingChannel());
  }

  // Installs a voter that asks `IsNeededGuestUrl` and re-checks every page
  // when `changes_` is notified.
  void InstallVoterWithChanges() {
    g_guest_needed = true;
    voter_ = std::make_unique<NexusGuestPriorityVoter>(
        base::BindRepeating(&IsNeededGuestUrl),
        base::BindRepeating(&base::RepeatingClosureList::Add,
                            base::Unretained(&changes_)));
    voter_->InitializeOnGraph(graph(), observer_.BuildVotingChannel());
  }

  // Installs a voter that skips the frames in `skipped_`, as the production
  // voter skips app-page frames.
  void InstallVoterWithSkips() {
    voter_ = std::make_unique<NexusGuestPriorityVoter>(
        base::BindRepeating(&IsGuestUrl),
        NexusGuestPriorityVoter::SubscribeCallback(),
        base::BindRepeating(
            [](const std::set<const FrameNode*>* skipped,
               const FrameNode* frame_node) {
              return skipped->contains(frame_node) ||
                     frame_node->GetBrowsingInstanceId() ==
                         kSkippedBrowsingInstance;
            },
            base::Unretained(&skipped_)));
    voter_->InitializeOnGraph(graph(), observer_.BuildVotingChannel());
  }

  base::RepeatingClosureList changes_;
  std::set<const FrameNode*> skipped_;

  bool HasGuestVote(const FrameNode* frame_node) const {
    return observer_.HasVote(
        voter_->voter_id(), ExecutionContext::From(frame_node),
        base::Process::Priority::kUserBlocking,
        NexusGuestPriorityVoter::kGuestPageReason);
  }

  DummyVoteObserver observer_;
  std::unique_ptr<NexusGuestPriorityVoter> voter_;
};

}  // namespace

// The guest's main frame and its cross-process child (the sandboxed frame that
// hosts the mod workers) are both voted foreground once the page is a guest.
TEST_F(NexusGuestPriorityVoterTest, VotesForEveryFrameOfAGuestPage) {
  InstallVoter();
  MockSinglePageWithMultipleProcessesGraph mock_graph(graph());
  EXPECT_EQ(observer_.GetVoteCount(), 0u);

  Commit(mock_graph.page.get(), kGuestUrl);

  EXPECT_EQ(observer_.GetVoteCount(), 2u);
  EXPECT_TRUE(HasGuestVote(mock_graph.frame.get()));
  EXPECT_TRUE(HasGuestVote(mock_graph.child_frame.get()));
}

// Any other page gets no vote.
TEST_F(NexusGuestPriorityVoterTest, IgnoresOtherPages) {
  InstallVoter();
  MockSinglePageWithMultipleProcessesGraph mock_graph(graph());

  Commit(mock_graph.page.get(), kOtherUrl);

  EXPECT_EQ(observer_.GetVoteCount(), 0u);
}

// A guest page that exists before the voter is installed is picked up.
TEST_F(NexusGuestPriorityVoterTest, VotesForGuestPagesPresentAtInstall) {
  MockSinglePageWithMultipleProcessesGraph mock_graph(graph());
  Commit(mock_graph.page.get(), kGuestUrl);

  InstallVoter();

  EXPECT_EQ(observer_.GetVoteCount(), 2u);
  EXPECT_TRUE(HasGuestVote(mock_graph.frame.get()));
  EXPECT_TRUE(HasGuestVote(mock_graph.child_frame.get()));
}

// A frame created later in a guest page (a mod host frame spawned after boot)
// is voted on arrival, and its vote goes when the frame goes.
TEST_F(NexusGuestPriorityVoterTest, FollowsFramesAddedAndRemoved) {
  InstallVoter();
  MockSinglePageInSingleProcessGraph mock_graph(graph());
  Commit(mock_graph.page.get(), kGuestUrl);
  ASSERT_EQ(observer_.GetVoteCount(), 1u);

  auto child = CreateFrameNodeAutoId(mock_graph.process.get(),
                                     mock_graph.page.get(),
                                     mock_graph.frame.get());
  EXPECT_EQ(observer_.GetVoteCount(), 2u);
  EXPECT_TRUE(HasGuestVote(child.get()));

  child.reset();
  EXPECT_EQ(observer_.GetVoteCount(), 1u);
  EXPECT_TRUE(HasGuestVote(mock_graph.frame.get()));
}

// An app-page frame of a guest gets no vote; the guest's other frames keep
// theirs.
TEST_F(NexusGuestPriorityVoterTest, SkipsAnAppPageFrameAndKeepsTheOthers) {
  InstallVoterWithSkips();
  MockSinglePageWithMultipleProcessesGraph mock_graph(graph());
  skipped_.insert(mock_graph.child_frame.get());

  Commit(mock_graph.page.get(), kGuestUrl);

  EXPECT_EQ(observer_.GetVoteCount(), 1u);
  EXPECT_TRUE(HasGuestVote(mock_graph.frame.get()));
  EXPECT_FALSE(HasGuestVote(mock_graph.child_frame.get()));
}

// Frames added later below an app-page frame get no vote either, while a
// sibling frame of the guest does.
TEST_F(NexusGuestPriorityVoterTest, FramesAddedBelowASkippedFrameGetNoVote) {
  InstallVoterWithSkips();
  MockSinglePageInSingleProcessGraph mock_graph(graph());
  Commit(mock_graph.page.get(), kGuestUrl);
  auto page_frame = CreateFrameNodeAutoId(mock_graph.process.get(),
                                          mock_graph.page.get(),
                                          mock_graph.frame.get());
  ASSERT_EQ(observer_.GetVoteCount(), 2u);
  skipped_.insert(page_frame.get());

  auto inner = CreateFrameNodeAutoId(mock_graph.process.get(),
                                     mock_graph.page.get(), page_frame.get());
  auto deeper = CreateFrameNodeAutoId(mock_graph.process.get(),
                                      mock_graph.page.get(), inner.get());
  auto kernel = CreateFrameNodeAutoId(mock_graph.process.get(),
                                      mock_graph.page.get(),
                                      mock_graph.frame.get());

  EXPECT_FALSE(HasGuestVote(inner.get()));
  EXPECT_FALSE(HasGuestVote(deeper.get()));
  EXPECT_TRUE(HasGuestVote(kernel.get()));
}

// A frame already skipped when it is added to a guest page gets no vote, nor
// does a frame added below it; a sibling frame does.
TEST_F(NexusGuestPriorityVoterTest, AFrameSkippedOnArrivalGetsNoVote) {
  InstallVoterWithSkips();
  MockSinglePageInSingleProcessGraph mock_graph(graph());
  Commit(mock_graph.page.get(), kGuestUrl);
  ASSERT_EQ(observer_.GetVoteCount(), 1u);

  auto page_frame = CreateFrameNodeAutoId(
      mock_graph.process.get(), mock_graph.page.get(), mock_graph.frame.get(),
      kSkippedBrowsingInstance);
  auto inner = CreateFrameNodeAutoId(mock_graph.process.get(),
                                     mock_graph.page.get(), page_frame.get());
  auto kernel = CreateFrameNodeAutoId(mock_graph.process.get(),
                                      mock_graph.page.get(),
                                      mock_graph.frame.get());

  EXPECT_FALSE(HasGuestVote(page_frame.get()));
  EXPECT_FALSE(HasGuestVote(inner.get()));
  EXPECT_TRUE(HasGuestVote(kernel.get()));
  EXPECT_EQ(observer_.GetVoteCount(), 2u);
}

// A frame skipped from the start is never voted, even when its page becomes
// a guest after the frame exists.
TEST_F(NexusGuestPriorityVoterTest, ASkippedSubtreeIsNotVotedOnReclassify) {
  InstallVoterWithSkips();
  MockSinglePageInSingleProcessGraph mock_graph(graph());
  auto page_frame = CreateFrameNodeAutoId(mock_graph.process.get(),
                                          mock_graph.page.get(),
                                          mock_graph.frame.get());
  auto inner = CreateFrameNodeAutoId(mock_graph.process.get(),
                                     mock_graph.page.get(), page_frame.get());
  skipped_.insert(page_frame.get());

  Commit(mock_graph.page.get(), kGuestUrl);

  EXPECT_EQ(observer_.GetVoteCount(), 1u);
  EXPECT_TRUE(HasGuestVote(mock_graph.frame.get()));
  EXPECT_FALSE(HasGuestVote(page_frame.get()));
  EXPECT_FALSE(HasGuestVote(inner.get()));
}

// Navigating away from the guest drops every vote of the page.
TEST_F(NexusGuestPriorityVoterTest, DropsVotesWhenThePageStopsBeingAGuest) {
  InstallVoter();
  MockSinglePageWithMultipleProcessesGraph mock_graph(graph());
  Commit(mock_graph.page.get(), kGuestUrl);
  ASSERT_EQ(observer_.GetVoteCount(), 2u);

  Commit(mock_graph.page.get(), kOtherUrl);

  EXPECT_EQ(observer_.GetVoteCount(), 0u);
}

// Destroying the guest page leaves no vote behind.
TEST_F(NexusGuestPriorityVoterTest, NoVotesOutliveThePage) {
  InstallVoter();
  auto mock_graph =
      std::make_unique<MockSinglePageWithMultipleProcessesGraph>(graph());
  Commit(mock_graph->page.get(), kGuestUrl);
  ASSERT_EQ(observer_.GetVoteCount(), 2u);

  mock_graph.reset();

  EXPECT_EQ(observer_.GetVoteCount(), 0u);
}

// Tearing the voter down withdraws its votes.
TEST_F(NexusGuestPriorityVoterTest, TearDownWithdrawsVotes) {
  InstallVoter();
  MockSinglePageWithMultipleProcessesGraph mock_graph(graph());
  Commit(mock_graph.page.get(), kGuestUrl);
  ASSERT_EQ(observer_.GetVoteCount(), 2u);

  voter_->TearDownOnGraph(graph());
  voter_.reset();

  EXPECT_EQ(observer_.GetVoteCount(), 0u);
}

// When the predicate's answer changes (a parked page no session needs, then
// needed again), a change notification moves every vote of the page.
TEST_F(NexusGuestPriorityVoterTest, ReclassifiesPagesOnChange) {
  InstallVoterWithChanges();
  MockSinglePageWithMultipleProcessesGraph mock_graph(graph());
  Commit(mock_graph.page.get(), kGuestUrl);
  ASSERT_EQ(observer_.GetVoteCount(), 2u);

  g_guest_needed = false;
  EXPECT_EQ(observer_.GetVoteCount(), 2u) << "no re-check without a change";
  changes_.Notify();
  EXPECT_EQ(observer_.GetVoteCount(), 0u);

  g_guest_needed = true;
  changes_.Notify();
  EXPECT_EQ(observer_.GetVoteCount(), 2u);
  EXPECT_TRUE(HasGuestVote(mock_graph.frame.get()));
  EXPECT_TRUE(HasGuestVote(mock_graph.child_frame.get()));
}

// After tear-down a change notification reaches nothing.
TEST_F(NexusGuestPriorityVoterTest, NoReclassificationAfterTearDown) {
  InstallVoterWithChanges();
  voter_->TearDownOnGraph(graph());
  voter_.reset();
  EXPECT_TRUE(changes_.empty());
}

// The production predicate rejects a page with no guest behind it.
TEST_F(NexusGuestPriorityVoterTest, ProductionPredicateRejectsNonGuestPage) {
  MockSinglePageInSingleProcessGraph mock_graph(graph());
  Commit(mock_graph.page.get(), kGuestUrl);

  EXPECT_FALSE(IsNexusGuestPage(mock_graph.page.get()));
}

}  // namespace nexus
