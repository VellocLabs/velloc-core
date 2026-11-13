// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/llm/nexus_egress_contexts.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/task_environment.h"
#include "content/public/test/browser_task_environment.h"
#include "custom_browser/browser/nexus/api/network/nexus_proxy_endpoint.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/base/auth.h"
#include "net/base/proxy_string_util.h"
#include "net/proxy_resolution/proxy_config.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "services/network/public/mojom/url_loader_network_service_observer.mojom.h"
#include "services/network/test/test_network_context.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace nexus {
namespace {

// A network context that records what it was configured with and keeps the
// observer of the last loader factory made on it.
class RecordingContext : public network::TestNetworkContext {
 public:
  RecordingContext(mojo::PendingReceiver<network::mojom::NetworkContext> receiver,
                   std::string proxy_rules)
      : proxy_rules(std::move(proxy_rules)),
        receiver_(this, std::move(receiver)) {}

  void CreateURLLoaderFactory(
      mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver,
      network::mojom::URLLoaderFactoryParamsPtr params) override {
    ++factory_count;
    trusted = params->is_trusted;
    observer.reset();
    observer.Bind(std::move(params->url_loader_network_observer));
    factory_receivers_.push_back(std::move(receiver));
  }

  void Disconnect() { receiver_.reset(); }

  const std::string proxy_rules;
  int factory_count = 0;
  bool trusted = false;
  mojo::Remote<network::mojom::URLLoaderNetworkServiceObserver> observer;

 private:
  mojo::Receiver<network::mojom::NetworkContext> receiver_;
  std::vector<mojo::PendingReceiver<network::mojom::URLLoaderFactory>>
      factory_receivers_;
};

// Records the credentials an auth challenge was answered with.
class RecordingResponder : public network::mojom::AuthChallengeResponder {
 public:
  mojo::PendingRemote<network::mojom::AuthChallengeResponder> Bind() {
    return receiver_.BindNewPipeAndPassRemote();
  }

  void OnAuthCredentials(
      const std::optional<net::AuthCredentials>& credentials) override {
    answered = true;
    answer = credentials;
  }

  bool answered = false;
  std::optional<net::AuthCredentials> answer;

 private:
  mojo::Receiver<network::mojom::AuthChallengeResponder> receiver_{this};
};

ProxyEndpoint Endpoint(const std::string& url) {
  return *ParseProxyEndpoint(url, ProxyCredentials::kAllowForHttp);
}

class NexusEgressContextsTest : public testing::Test {
 protected:
  NexusEgressContextsTest() {
    contexts_.SetContextBinderForTesting(base::BindRepeating(
        &NexusEgressContextsTest::BindContext, base::Unretained(this)));
  }

  void BindContext(mojo::PendingReceiver<network::mojom::NetworkContext> receiver,
                   const net::ProxyConfigWithAnnotation& config) {
    bound_.push_back(std::make_unique<RecordingContext>(
        std::move(receiver),
        net::ProxyServerToProxyUri(
            config.value().proxy_rules().single_proxies.First().First())));
  }

  // The answer of the newest context's observer to a challenge.
  std::optional<net::AuthCredentials> Challenge(bool is_proxy,
                                                bool first_attempt) {
    task_environment_.RunUntilIdle();
    net::AuthChallengeInfo info;
    info.is_proxy = is_proxy;
    RecordingResponder responder;
    bound_.back()->observer->OnAuthRequired(
        std::nullopt, 1, GURL("https://api.example.com/"), first_attempt, info,
        nullptr, responder.Bind());
    task_environment_.RunUntilIdle();
    EXPECT_TRUE(responder.answered);
    return responder.answer;
  }

  content::BrowserTaskEnvironment task_environment_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  std::vector<std::unique_ptr<RecordingContext>> bound_;
  NexusEgressContexts contexts_;
};

TEST_F(NexusEgressContextsTest, AContextCarriesItsProxyAsItsOnlyConfiguration) {
  auto lease = contexts_.Acquire(Endpoint("socks5://proxy.example.com:1080"));
  ASSERT_TRUE(lease.has_value());
  EXPECT_TRUE(lease->url_loader_factory);
  task_environment_.RunUntilIdle();
  ASSERT_EQ(bound_.size(), 1u);
  EXPECT_EQ(bound_[0]->proxy_rules, "socks5://proxy.example.com:1080");
  EXPECT_EQ(bound_[0]->factory_count, 1);
  EXPECT_TRUE(bound_[0]->trusted);
}

TEST_F(NexusEgressContextsTest, RequestsThroughOneProxyShareOneContext) {
  auto first = contexts_.Acquire(Endpoint("http://a.example.com:8080"));
  auto second = contexts_.Acquire(Endpoint("HTTP://a.example.com:8080/"));
  auto other = contexts_.Acquire(Endpoint("http://b.example.com:8080"));
  auto credentialed =
      contexts_.Acquire(Endpoint("http://user:pass@a.example.com:8080"));
  task_environment_.RunUntilIdle();
  EXPECT_EQ(bound_.size(), 3u);
  EXPECT_EQ(contexts_.context_count_for_testing(), 3u);
  EXPECT_EQ(bound_[0]->factory_count, 2);
}

TEST_F(NexusEgressContextsTest, AnIdleContextIsDroppedAfterItsLifetime) {
  auto held = contexts_.Acquire(Endpoint("http://held.example.com"));
  {
    auto released = contexts_.Acquire(Endpoint("http://idle.example.com"));
  }
  EXPECT_EQ(contexts_.context_count_for_testing(), 2u);

  task_environment_.FastForwardBy(NexusEgressContexts::kIdleLifetime);
  auto next = contexts_.Acquire(Endpoint("http://next.example.com"));
  // The idle one went; the one still carrying a request stayed.
  EXPECT_EQ(contexts_.context_count_for_testing(), 2u);
  auto again = contexts_.Acquire(Endpoint("http://held.example.com"));
  task_environment_.RunUntilIdle();
  EXPECT_EQ(bound_.size(), 3u);
}

TEST_F(NexusEgressContextsTest, AtTheLimitAnIdleContextMakesRoomABusyOneDoesNot) {
  std::vector<NexusEgressContexts::Lease> held;
  for (size_t i = 0; i < NexusEgressContexts::kMaxContexts; ++i) {
    auto lease = contexts_.Acquire(
        Endpoint("http://p" + base::NumberToString(i) + ".example.com"));
    ASSERT_TRUE(lease.has_value());
    held.push_back(std::move(*lease));
  }
  EXPECT_FALSE(contexts_.Acquire(Endpoint("http://extra.example.com")));
  // A proxy that already has a context is still served at the limit.
  EXPECT_TRUE(contexts_.Acquire(Endpoint("http://p0.example.com")));

  held.erase(held.begin() + 3);
  EXPECT_TRUE(contexts_.Acquire(Endpoint("http://extra.example.com")));
  EXPECT_EQ(contexts_.context_count_for_testing(),
            NexusEgressContexts::kMaxContexts);
}

TEST_F(NexusEgressContextsTest, ALostContextIsMadeAgainOnNextUse) {
  auto first = contexts_.Acquire(Endpoint("http://proxy.example.com"));
  task_environment_.RunUntilIdle();
  ASSERT_EQ(bound_.size(), 1u);
  bound_[0]->Disconnect();
  task_environment_.RunUntilIdle();

  auto second = contexts_.Acquire(Endpoint("http://proxy.example.com"));
  task_environment_.RunUntilIdle();
  ASSERT_EQ(bound_.size(), 2u);
  EXPECT_EQ(bound_[1]->proxy_rules, bound_[0]->proxy_rules);
  EXPECT_EQ(contexts_.context_count_for_testing(), 1u);
}

TEST_F(NexusEgressContextsTest, OnlyTheProxysFirstChallengeIsAnswered) {
  auto lease =
      contexts_.Acquire(Endpoint("http://us%40er:s3cret@proxy.example.com"));
  const auto answer = Challenge(/*is_proxy=*/true, /*first_attempt=*/true);
  ASSERT_TRUE(answer.has_value());
  EXPECT_EQ(answer->username(), u"us@er");
  EXPECT_EQ(answer->password(), u"s3cret");

  EXPECT_FALSE(Challenge(/*is_proxy=*/true, /*first_attempt=*/false));
  // The target server's own challenge never receives the proxy's credentials.
  EXPECT_FALSE(Challenge(/*is_proxy=*/false, /*first_attempt=*/true));
}

TEST_F(NexusEgressContextsTest, AProxyWithoutCredentialsAnswersNoChallenge) {
  auto lease = contexts_.Acquire(Endpoint("http://proxy.example.com"));
  EXPECT_FALSE(Challenge(/*is_proxy=*/true, /*first_attempt=*/true));
}

}  // namespace
}  // namespace nexus
