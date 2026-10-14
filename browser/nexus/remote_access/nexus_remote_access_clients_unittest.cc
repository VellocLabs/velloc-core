// Copyright 2026 VellocLabs
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/remote_access/nexus_remote_access_clients.h"

#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace nexus::remote_access {
namespace {

class RecordingObserver : public ConnectedClientsObserver {
 public:
  void OnConnectedClientsChanged(const ConnectedClients& summary) override {
    seen.push_back(summary);
  }
  std::vector<ConnectedClients> seen;
};

class NexusRemoteAccessClientsTest : public testing::Test {
 protected:
  void SetUp() override { ResetConnectedClientsForTesting(); }
  void TearDown() override { ResetConnectedClientsForTesting(); }

  // Two stand-ins for two profiles' services.
  const int profile_a_ = 0;
  const int profile_b_ = 0;
};

TEST_F(NexusRemoteAccessClientsTest, StartsEmpty) {
  EXPECT_EQ(GetConnectedClients(), ConnectedClients());
}

TEST_F(NexusRemoteAccessClientsTest, SumsClientsAndOrsListeningOverSources) {
  ReportConnectedClients(&profile_a_, {.listening = true, .clients = 2});
  ReportConnectedClients(&profile_b_, {.listening = false, .clients = 0});
  EXPECT_EQ(GetConnectedClients(),
            (ConnectedClients{.listening = true, .clients = 2}));

  ReportConnectedClients(&profile_b_, {.listening = true, .clients = 1});
  EXPECT_EQ(GetConnectedClients(),
            (ConnectedClients{.listening = true, .clients = 3}));
}

TEST_F(NexusRemoteAccessClientsTest, AReportReplacesThatSourcesPreviousOne) {
  ReportConnectedClients(&profile_a_, {.listening = true, .clients = 2});
  ReportConnectedClients(&profile_a_, {.listening = true, .clients = 1});
  EXPECT_EQ(GetConnectedClients().clients, 1);
}

TEST_F(NexusRemoteAccessClientsTest, ForgetDropsTheSource) {
  ReportConnectedClients(&profile_a_, {.listening = true, .clients = 2});
  ForgetConnectedClients(&profile_a_);
  EXPECT_EQ(GetConnectedClients(), ConnectedClients());
}

TEST_F(NexusRemoteAccessClientsTest, ObserversHearOnlyChangesOfTheSummary) {
  RecordingObserver observer;
  AddConnectedClientsObserver(&observer);

  ReportConnectedClients(&profile_a_, {.listening = true, .clients = 0});
  // Nothing visible moves: the same report again, and a second source's 0.
  ReportConnectedClients(&profile_a_, {.listening = true, .clients = 0});
  ReportConnectedClients(&profile_b_, {.listening = false, .clients = 0});
  ReportConnectedClients(&profile_a_, {.listening = true, .clients = 1});

  ASSERT_EQ(observer.seen.size(), 2u);
  EXPECT_EQ(observer.seen[0],
            (ConnectedClients{.listening = true, .clients = 0}));
  EXPECT_EQ(observer.seen[1],
            (ConnectedClients{.listening = true, .clients = 1}));

  RemoveConnectedClientsObserver(&observer);
  ReportConnectedClients(&profile_a_, {.listening = true, .clients = 5});
  EXPECT_EQ(observer.seen.size(), 2u);
}

}  // namespace
}  // namespace nexus::remote_access
