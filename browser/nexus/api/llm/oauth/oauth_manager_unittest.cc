// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/llm/oauth/oauth_manager.h"

#include <optional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/testing_pref_service.h"
#include "custom_browser/browser/nexus/api/llm/llm_pref_names.h"
#include "mojo/core/embedder/embedder.h"
#include "services/network/public/cpp/weak_wrapper_shared_url_loader_factory.h"
#include "services/network/test/test_url_loader_factory.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

constexpr char kProvider[] = "qwen-code";
constexpr char kDeviceCodeUrl[] =
    "https://chat.qwen.ai/api/v1/oauth2/device/code";
constexpr char kTokenUrl[] = "https://chat.qwen.ai/api/v1/oauth2/token";

struct Done {
  bool ok = false;
  std::string error;
};

class NexusOAuthManagerDeviceCodeTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    // SimpleURLLoader talks to the test factory over Mojo pipes.
    static bool initialized = false;
    if (!initialized) {
      mojo::core::Init();
      initialized = true;
    }
  }

  void SetUp() override {
    prefs_.registry()->RegisterDictionaryPref(prefs::kNexusProviderConfigs);
    manager_ = std::make_unique<NexusOAuthManager>(
        &prefs_, factory_.GetSafeWeakWrapper(),
        base::BindRepeating(
            [](int* saved, const std::string&) { ++*saved; }, &saved_));
  }

  // Starts a login whose code expires after `expires_in` seconds and whose
  // poll interval is 5 s; `done` records its end.
  void Start(std::optional<Done>* done, int expires_in = 900) {
    factory_.AddResponse(
        kDeviceCodeUrl,
        R"({"device_code":"dev","user_code":"ABCD-1234",)"
        R"("verification_uri":"https://chat.qwen.ai/authorize",)"
        R"("interval":5,"expires_in":)" +
            std::to_string(expires_in) + "}");
    bool started = false;
    manager_->StartDeviceCodeFlow(
        kProvider,
        base::BindOnce(
            [](bool* started, bool ok, NexusOAuthManager::DeviceCodeInfo info,
               std::string) {
              *started = ok;
              EXPECT_EQ("ABCD-1234", info.user_code);
            },
            &started),
        base::BindOnce(
            [](std::optional<Done>* done, bool ok, std::string error) {
              *done = Done{ok, std::move(error)};
            },
            done));
    task_environment_.RunUntilIdle();
    ASSERT_TRUE(started);
  }

  void AnswerPoll(const std::string& body) {
    factory_.AddResponse(kTokenUrl, body);
  }

  void Poll() { task_environment_.FastForwardBy(base::Seconds(5)); }

  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  network::TestURLLoaderFactory factory_;
  TestingPrefServiceSimple prefs_;
  int saved_ = 0;
  std::unique_ptr<NexusOAuthManager> manager_;
};

TEST_F(NexusOAuthManagerDeviceCodeTest, DoneReportsTheSavedTokens) {
  std::optional<Done> done;
  Start(&done);
  AnswerPoll(R"({"error":"authorization_pending"})");
  Poll();
  EXPECT_FALSE(done.has_value());

  AnswerPoll(R"({"access_token":"access-token"})");
  Poll();
  ASSERT_TRUE(done.has_value());
  EXPECT_TRUE(done->ok);
  EXPECT_EQ(1, saved_);
  EXPECT_TRUE(manager_->IsConnected(kProvider));
}

TEST_F(NexusOAuthManagerDeviceCodeTest, DoneReportsTheServersRefusal) {
  std::optional<Done> done;
  Start(&done);
  AnswerPoll(R"({"error":"access_denied"})");
  Poll();
  ASSERT_TRUE(done.has_value());
  EXPECT_FALSE(done->ok);
  EXPECT_EQ("access_denied", done->error);
  EXPECT_EQ(0, saved_);
}

TEST_F(NexusOAuthManagerDeviceCodeTest, DoneReportsAnExpiredCodeWithNoError) {
  std::optional<Done> done;
  Start(&done, /*expires_in=*/8);
  AnswerPoll(R"({"error":"authorization_pending"})");
  Poll();
  EXPECT_FALSE(done.has_value());
  Poll();
  ASSERT_TRUE(done.has_value());
  EXPECT_FALSE(done->ok);
  EXPECT_EQ("", done->error);
}

TEST_F(NexusOAuthManagerDeviceCodeTest, CancelStopsPollingAndNeverRunsDone) {
  std::optional<Done> done;
  Start(&done);
  manager_->CancelDeviceCodeFlow(kProvider);
  AnswerPoll(R"({"access_token":"access-token"})");
  Poll();
  Poll();
  EXPECT_FALSE(done.has_value());
  EXPECT_EQ(0, saved_);
}

TEST_F(NexusOAuthManagerDeviceCodeTest, ANewLoginSupersedesTheEarlierOne) {
  std::optional<Done> first;
  Start(&first);
  std::optional<Done> second;
  Start(&second);

  AnswerPoll(R"({"access_token":"access-token"})");
  Poll();
  EXPECT_FALSE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_TRUE(second->ok);
  // Only the current login polled to the end.
  EXPECT_EQ(1, saved_);
}

}  // namespace
}  // namespace nexus
