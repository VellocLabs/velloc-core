// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/llm/nexus_llm_bridge_adapter.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "chrome/browser/prefs/browser_prefs.h"
#include "chrome/test/base/testing_profile.h"
#include "components/prefs/pref_service.h"
#include "components/sync_preferences/testing_pref_service_syncable.h"
#include "content/public/test/browser_task_environment.h"
#include "custom_browser/browser/nexus/api/llm/llm_broker_factory.h"
#include "custom_browser/browser/nexus/api/llm/llm_pref_names.h"
#include "custom_browser/browser/nexus/nexus_bridge_response_delegate.h"
#include "custom_browser/browser/nexus/nexus_test_support.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "services/network/public/cpp/weak_wrapper_shared_url_loader_factory.h"
#include "services/network/test/test_url_loader_factory.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

constexpr char kProviderId[] = "relay-test";
constexpr char kBaseUrl[] = "https://api.relay.test/v1";
constexpr char kStreamUrl[] = "https://api.relay.test/v1/chat";

class RecordingDelegate : public NexusBridgeResponseDelegate {
 public:
  int success_count = 0;
  int error_count = 0;
  std::string last_error_code;

  void SendSuccessResponse(const std::string& /*id*/,
                           const base::DictValue& /*route*/,
                           base::Value /*result*/) override {
    ++success_count;
  }
  void SendBinaryResponse(const std::string& /*id*/,
                          const base::DictValue& /*route*/,
                          base::Value /*result*/,
                          mojo_base::BigBuffer /*data*/,
                          std::string_view /*result_binary_field*/) override {
    ++success_count;
  }
  void SendErrorResponse(const std::string& /*id*/,
                         const base::DictValue& /*route*/,
                         const std::string& code,
                         const std::string& /*message*/) override {
    ++error_count;
    last_error_code = code;
  }
  void SendEvent(const base::DictValue& /*route*/,
                 base::Value /*payload*/) override {}
};

base::DictValue ModelRoute(std::string_view action) {
  base::DictValue route;
  route.Set("namespace", "nexus");
  route.Set("capability", "model");
  route.Set("action", action);
  return route;
}

// The raw-stream slot table: a stream whose reader is gone must not hold a
// slot until the browser restarts.
class NexusLlmRawStreamSlotsTest : public testing::Test {
 protected:
  void SetUp() override {
    LlmBrokerFactory::GetInstance();
    auto prefs =
        std::make_unique<sync_preferences::TestingPrefServiceSyncable>();
    RegisterUserProfilePrefs(prefs->registry());
    TestingProfile::Builder builder;
    builder.SetPrefService(std::move(prefs));
    test::AddGlicDisablingFactory(builder);
    profile_ = builder.Build();

    base::DictValue config;
    config.Set("base_url", kBaseUrl);
    base::DictValue configs;
    configs.Set(kProviderId, std::move(config));
    profile_->GetPrefs()->SetDict(prefs::kNexusProviderConfigs,
                                  std::move(configs));

    adapter_ = std::make_unique<NexusLlmBridgeAdapter>(&delegate_,
                                                       profile_.get());
    adapter_->SetURLLoaderFactoryForTesting(factory_.GetSafeWeakWrapper());
  }

  void TearDown() override {
    adapter_.reset();
    profile_.reset();
  }

  void Dispatch(std::string_view action, base::DictValue payload,
                bool run_tasks = true) {
    EXPECT_TRUE(
        adapter_->Dispatch(action, "req", ModelRoute(action), payload, {}));
    if (run_tasks) {
      task_environment_.RunUntilIdle();
    }
  }

  // An open whose response is answered at once (`with_response`) or never.
  void Open(int n, bool with_response, bool run_tasks = true) {
    const std::string url = std::string(kStreamUrl) + "?n=" +
                            base::NumberToString(n);
    if (with_response) {
      factory_.AddResponse(url, "data: [DONE]\n\n");
    }
    base::DictValue payload;
    payload.Set("providerId", kProviderId);
    payload.Set("url", url);
    base::ListValue body;
    body.Append(123);
    payload.Set("body", std::move(body));
    payload.Set("streamId", "stream-" + base::NumberToString(1000 + n));
    Dispatch("rawStreamOpen", std::move(payload), run_tasks);
  }

  content::BrowserTaskEnvironment task_environment_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  network::TestURLLoaderFactory factory_;
  RecordingDelegate delegate_;
  std::unique_ptr<TestingProfile> profile_;
  std::unique_ptr<NexusLlmBridgeAdapter> adapter_;
};

TEST_F(NexusLlmRawStreamSlotsTest, ClosingTheConnectionReleasesEverySlot) {
  for (int n = 0; n < 3; ++n) {
    Open(n, /*with_response=*/false);
  }
  EXPECT_EQ(adapter_->raw_stream_count_for_testing(), 3u);
  adapter_->OnBridgeConnectionClosed();
  EXPECT_EQ(adapter_->raw_stream_count_for_testing(), 0u);
}

TEST_F(NexusLlmRawStreamSlotsTest, ClosingTheConnectionForgetsEarlyCancels) {
  base::DictValue cancel;
  cancel.Set("streamId", "stream-1000");
  Dispatch("rawStreamCancel", std::move(cancel));
  adapter_->OnBridgeConnectionClosed();
  Open(0, /*with_response=*/true);
  EXPECT_EQ(delegate_.error_count, 0);
  EXPECT_EQ(adapter_->raw_stream_count_for_testing(), 1u);
}

TEST_F(NexusLlmRawStreamSlotsTest, PostedEraseCannotRemoveReconnectedStream) {
  Open(0, /*with_response=*/true);
  base::DictValue read;
  read.Set("streamId", "stream-1000");
  Dispatch("rawStreamRead", std::move(read), /*run_tasks=*/false);
  ASSERT_EQ(adapter_->raw_stream_count_for_testing(), 1u);
  adapter_->OnBridgeConnectionClosed();
  Open(0, /*with_response=*/false, /*run_tasks=*/false);
  ASSERT_EQ(adapter_->raw_stream_count_for_testing(), 1u);
  task_environment_.RunUntilIdle();
  EXPECT_EQ(adapter_->raw_stream_count_for_testing(), 1u);
  base::DictValue next_read;
  next_read.Set("streamId", "stream-1000");
  Dispatch("rawStreamRead", std::move(next_read));
  EXPECT_EQ(delegate_.error_count, 0);
}

TEST_F(NexusLlmRawStreamSlotsTest, PostedEraseCannotRemoveReclaimedReplacement) {
  for (size_t n = 0; n < NexusLlmBridgeAdapter::kMaxRawStreams; ++n) {
    Open(static_cast<int>(n), /*with_response=*/true);
  }
  task_environment_.FastForwardBy(
      NexusLlmBridgeAdapter::kAbandonedRawStreamGrace + base::Seconds(1));
  base::DictValue read;
  read.Set("streamId", "stream-1000");
  Dispatch("rawStreamRead", std::move(read), /*run_tasks=*/false);
  Open(0, /*with_response=*/false, /*run_tasks=*/false);
  ASSERT_EQ(adapter_->raw_stream_count_for_testing(), 1u);
  task_environment_.RunUntilIdle();
  EXPECT_EQ(adapter_->raw_stream_count_for_testing(), 1u);
  EXPECT_EQ(delegate_.error_count, 0);
}

// The guest's runtime was torn down mid-response, so neither the final read
// nor the cancel ever arrives: the ended streams must not lock every later
// request, of any provider, out of the table.
TEST_F(NexusLlmRawStreamSlotsTest, EndedStreamsWithNoReaderAreReclaimed) {
  const size_t cap = NexusLlmBridgeAdapter::kMaxRawStreams;
  for (size_t n = 0; n < cap; ++n) {
    Open(static_cast<int>(n), /*with_response=*/true);
  }
  ASSERT_EQ(adapter_->raw_stream_count_for_testing(), cap);
  ASSERT_EQ(delegate_.error_count, 0);

  // Within the grace a full table still refuses: a slow reader keeps its
  // stream.
  Open(100, /*with_response=*/true);
  EXPECT_EQ(delegate_.last_error_code, "TOO_MANY_STREAMS");

  task_environment_.FastForwardBy(
      NexusLlmBridgeAdapter::kAbandonedRawStreamGrace + base::Seconds(1));
  const int errors_before = delegate_.error_count;
  Open(101, /*with_response=*/true);
  EXPECT_EQ(delegate_.error_count, errors_before);
  EXPECT_EQ(adapter_->raw_stream_count_for_testing(), 1u);
}

// A stream still receiving its response is never reclaimed, however long it
// runs: only the idle timeout or a cancel ends it.
TEST_F(NexusLlmRawStreamSlotsTest, RunningStreamsAreNotReclaimed) {
  const size_t cap = NexusLlmBridgeAdapter::kMaxRawStreams;
  for (size_t n = 0; n < cap; ++n) {
    Open(static_cast<int>(n), /*with_response=*/false);
  }
  ASSERT_EQ(adapter_->raw_stream_count_for_testing(), cap);
  task_environment_.FastForwardBy(
      NexusLlmBridgeAdapter::kAbandonedRawStreamGrace / 2);
  Open(100, /*with_response=*/true);
  EXPECT_EQ(delegate_.last_error_code, "TOO_MANY_STREAMS");
  EXPECT_EQ(adapter_->raw_stream_count_for_testing(), cap);
}

}  // namespace
}  // namespace nexus
