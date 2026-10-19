// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/boot_bundle/boot_bundle_url_loader.h"

#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/test/task_environment.h"
#include "custom_browser/browser/nexus/boot_bundle/boot_bundle_index.h"
#include "custom_browser/browser/nexus/boot_bundle/boot_bundle_store.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "mojo/public/cpp/system/data_pipe_utils.h"
#include "net/base/load_flags.h"
#include "net/base/net_errors.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_util.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "services/network/test/test_url_loader_client.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace nexus {
namespace {

constexpr char kGuestUrl[] = "https://app.velloc.app/v1/";
constexpr char kChunkUrl[] = "https://app.velloc.app/v1/assets/main-abc123.js";
constexpr char kOtherUrl[] = "https://app.velloc.app/v1/managed-binaries.json";
constexpr int64_t kBuildTimeMs = 1758774728561;

// Stands in for the network-side factory: a request carrying
// LOAD_ONLY_FROM_CACHE answers from `cached` or fails with ERR_CACHE_MISS;
// any other request answers "network".
class FakeTargetFactory : public network::mojom::URLLoaderFactory {
 public:
  void CreateLoaderAndStart(
      mojo::PendingReceiver<network::mojom::URLLoader> loader,
      int32_t request_id,
      uint32_t options,
      const network::ResourceRequest& request,
      mojo::PendingRemote<network::mojom::URLLoaderClient> client,
      const net::MutableNetworkTrafficAnnotationTag& traffic_annotation)
      override {
    load_flags.push_back(request.load_flags);
    loaders_.push_back(std::move(loader));
    mojo::Remote<network::mojom::URLLoaderClient> remote(std::move(client));
    const bool cache_only = request.load_flags & net::LOAD_ONLY_FROM_CACHE;
    if (cache_only && !cached.contains(request.url.spec())) {
      remote->OnComplete(
          network::URLLoaderCompletionStatus(net::ERR_CACHE_MISS));
      return;
    }
    Respond(remote, cache_only ? "cached" : "network");
  }

  void Clone(mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver)
      override {
    receivers_.Add(this, std::move(receiver));
  }

  mojo::PendingRemote<network::mojom::URLLoaderFactory> BindRemote() {
    mojo::PendingRemote<network::mojom::URLLoaderFactory> remote;
    receivers_.Add(this, remote.InitWithNewPipeAndPassReceiver());
    return remote;
  }

  std::set<std::string> cached;
  std::vector<int> load_flags;

 private:
  static void Respond(mojo::Remote<network::mojom::URLLoaderClient>& client,
                      const std::string& body) {
    auto head = network::mojom::URLResponseHead::New();
    head->headers = base::MakeRefCounted<net::HttpResponseHeaders>(
        net::HttpUtil::AssembleRawHeaders("HTTP/1.1 200 OK\n\n"));
    mojo::ScopedDataPipeProducerHandle producer;
    mojo::ScopedDataPipeConsumerHandle consumer;
    ASSERT_EQ(mojo::CreateDataPipe(body.size(), producer, consumer),
              MOJO_RESULT_OK);
    ASSERT_EQ(producer->WriteAllData(base::as_byte_span(body)),
              MOJO_RESULT_OK);
    client->OnReceiveResponse(std::move(head), std::move(consumer),
                              std::nullopt);
    client->OnComplete(network::URLLoaderCompletionStatus(net::OK));
  }

  mojo::ReceiverSet<network::mojom::URLLoaderFactory> receivers_;
  std::vector<mojo::PendingReceiver<network::mojom::URLLoader>> loaders_;
};

class BootBundleURLLoaderTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
    data_path_ = temp_dir_.GetPath().AppendASCII("nexus_boot_bundle.dat");
    ASSERT_TRUE(base::WriteFile(data_path_, "<html>okconsole.log(1)"));
    std::optional<BootBundleIndex> index = BootBundleIndex::Parse(
        R"({"format":1,"buildTimeMs":1758774728561,"base":"/v1/",
            "origins":["https://app.velloc.app"],"files":[
              {"path":"index.html","offset":0,"size":8,"type":"text/html"},
              {"path":"assets/main-abc123.js","offset":8,"size":14,
               "type":"text/javascript"}]})",
        22);
    ASSERT_TRUE(index);
    store_.SetForTesting(std::move(*index), data_path_);
    CreateBootBundleURLLoaderFactory(&store_,
                                     factory_.BindNewPipeAndPassReceiver(),
                                     target_.BindRemote());
  }

  struct Result {
    int error = net::ERR_FAILED;
    std::string body;
    base::Time response_time;
  };

  Result Fetch(const char* url, int load_flags = 0) {
    network::ResourceRequest request;
    request.url = GURL(url);
    request.method = "GET";
    request.load_flags = load_flags;
    network::TestURLLoaderClient client;
    mojo::Remote<network::mojom::URLLoader> loader;
    factory_->CreateLoaderAndStart(
        loader.BindNewPipeAndPassReceiver(), 0, 0, request,
        client.CreateRemote(),
        net::MutableNetworkTrafficAnnotationTag(TRAFFIC_ANNOTATION_FOR_TESTS));
    client.RunUntilComplete();
    Result result;
    result.error = client.completion_status().error_code;
    if (client.response_head()) {
      result.response_time = client.response_head()->response_time;
    }
    if (client.response_body()) {
      mojo::BlockingCopyToString(client.response_body_release(),
                                 &result.body);
    }
    return result;
  }

  base::test::TaskEnvironment task_environment_;
  base::ScopedTempDir temp_dir_;
  base::FilePath data_path_;
  BootBundleStore store_;
  FakeTargetFactory target_;
  mojo::Remote<network::mojom::URLLoaderFactory> factory_;
};

TEST_F(BootBundleURLLoaderTest, CacheHitIsServedFromTheCache) {
  target_.cached.insert(kChunkUrl);
  Result result = Fetch(kChunkUrl);
  EXPECT_EQ(result.error, net::OK);
  EXPECT_EQ(result.body, "cached");
  ASSERT_EQ(target_.load_flags.size(), 1u);
  EXPECT_TRUE(target_.load_flags[0] & net::LOAD_ONLY_FROM_CACHE);
  EXPECT_TRUE(target_.load_flags[0] & net::LOAD_SKIP_CACHE_VALIDATION);
}

TEST_F(BootBundleURLLoaderTest, CacheMissIsServedFromTheBundle) {
  Result result = Fetch(kGuestUrl);
  EXPECT_EQ(result.error, net::OK);
  EXPECT_EQ(result.body, "<html>ok");
  EXPECT_EQ(result.response_time.InMillisecondsSinceUnixEpoch(),
            kBuildTimeMs);
  // Only the cache was asked: the network never saw the request.
  EXPECT_EQ(target_.load_flags.size(), 1u);

  Result chunk = Fetch(kChunkUrl);
  EXPECT_EQ(chunk.body, "console.log(1)");
}

TEST_F(BootBundleURLLoaderTest, UnreadableBundleFallsBackToTheNetwork) {
  ASSERT_TRUE(base::DeleteFile(data_path_));
  Result result = Fetch(kChunkUrl);
  EXPECT_EQ(result.error, net::OK);
  EXPECT_EQ(result.body, "network");
  ASSERT_EQ(target_.load_flags.size(), 2u);
  EXPECT_EQ(target_.load_flags[1], 0);
}

TEST_F(BootBundleURLLoaderTest, NetworkAndCacheOnlyModesPassThroughUnchanged) {
  for (int flags : {net::LOAD_VALIDATE_CACHE, net::LOAD_BYPASS_CACHE,
                    net::LOAD_DISABLE_CACHE}) {
    EXPECT_EQ(Fetch(kGuestUrl, flags).body, "network");
    EXPECT_EQ(target_.load_flags.back(), flags);
  }
  EXPECT_EQ(Fetch(kGuestUrl, net::LOAD_ONLY_FROM_CACHE).error,
            net::ERR_CACHE_MISS);
  EXPECT_EQ(target_.load_flags.size(), 4u);
}

TEST_F(BootBundleURLLoaderTest, ForceCacheMissIsServedFromTheBundle) {
  Result result = Fetch(kChunkUrl, net::LOAD_SKIP_CACHE_VALIDATION);
  EXPECT_EQ(result.body, "console.log(1)");
  ASSERT_EQ(target_.load_flags.size(), 1u);
  EXPECT_TRUE(target_.load_flags[0] & net::LOAD_ONLY_FROM_CACHE);
}

TEST_F(BootBundleURLLoaderTest, UnbundledUrlsPassThroughUnchanged) {
  Result result = Fetch(kOtherUrl);
  EXPECT_EQ(result.body, "network");
  ASSERT_EQ(target_.load_flags.size(), 1u);
  EXPECT_EQ(target_.load_flags[0], 0);
}

}  // namespace
}  // namespace nexus
