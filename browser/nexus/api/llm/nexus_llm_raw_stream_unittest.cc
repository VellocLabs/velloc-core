// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/llm/nexus_llm_raw_stream.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/run_loop.h"
#include "base/test/task_environment.h"
#include "net/url_request/redirect_info.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/cpp/weak_wrapper_shared_url_loader_factory.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "services/network/test/test_url_loader_factory.h"
#include "services/network/test/test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace nexus {
namespace {

constexpr char kUrl[] = "https://api.example.com/v1/stream";

class NexusLlmRawStreamTest : public testing::Test {
 protected:
  std::unique_ptr<NexusLlmRawStream> MakeStream(
      size_t max_bytes = 1024,
      NexusLlmRawStream::BodyTransform transform = {},
      NexusLlmRawStream::RedirectCheck redirect_check = {}) {
    auto request = std::make_unique<network::ResourceRequest>();
    request->url = GURL(kUrl);
    request->method = "POST";
    return std::make_unique<NexusLlmRawStream>(
        factory_.GetSafeWeakWrapper(), std::move(request),
        std::string("req-body"), std::string("application/json"), max_bytes, base::Seconds(30),
        std::move(redirect_check), std::vector<std::string>{"x-api-key"},
        std::move(transform));
  }

  NexusLlmRawStream::OpenResult Open(NexusLlmRawStream* stream) {
    std::optional<NexusLlmRawStream::OpenResult> result;
    stream->Start(base::BindOnce(
        [](std::optional<NexusLlmRawStream::OpenResult>* out,
           NexusLlmRawStream::OpenResult r) { *out = std::move(r); },
        &result));
    task_environment_.RunUntilIdle();
    EXPECT_TRUE(result.has_value());
    return result ? std::move(*result) : NexusLlmRawStream::OpenResult();
  }

  NexusLlmRawStream::ReadResult Read(NexusLlmRawStream* stream,
                                     size_t max_bytes = 1024) {
    std::optional<NexusLlmRawStream::ReadResult> result;
    stream->Read(base::TimeDelta(), max_bytes,
                 base::BindOnce(
                     [](std::optional<NexusLlmRawStream::ReadResult>* out,
                        NexusLlmRawStream::ReadResult r) {
                       *out = std::move(r);
                     },
                     &result));
    task_environment_.RunUntilIdle();
    EXPECT_TRUE(result.has_value());
    return result ? std::move(*result) : NexusLlmRawStream::ReadResult();
  }

  base::test::SingleThreadTaskEnvironment task_environment_;
  network::TestURLLoaderFactory factory_;
};

TEST_F(NexusLlmRawStreamTest, StreamsBodyInOrderAndReportsHead) {
  factory_.AddResponse(kUrl, "data: {\"a\":1}\n\ndata: [DONE]\n\n");
  auto stream = MakeStream();
  NexusLlmRawStream::OpenResult open = Open(stream.get());
  EXPECT_TRUE(open.error_code.empty()) << open.error_message;
  EXPECT_EQ(open.status, 200);

  NexusLlmRawStream::ReadResult first = Read(stream.get(), 6);
  EXPECT_EQ(first.data, "data: ");
  EXPECT_FALSE(first.done);
  NexusLlmRawStream::ReadResult rest = Read(stream.get());
  EXPECT_EQ(rest.data, "{\"a\":1}\n\ndata: [DONE]\n\n");
  EXPECT_TRUE(rest.done);
  EXPECT_TRUE(rest.error_code.empty());
}

TEST_F(NexusLlmRawStreamTest, ErrorStatusBodiesStillStream) {
  factory_.AddResponse(kUrl, "{\"error\":{\"message\":\"bad key\"}}",
                       net::HTTP_UNAUTHORIZED);
  auto stream = MakeStream();
  EXPECT_EQ(Open(stream.get()).status, 401);
  NexusLlmRawStream::ReadResult read = Read(stream.get());
  EXPECT_EQ(read.data, "{\"error\":{\"message\":\"bad key\"}}");
  EXPECT_TRUE(read.done);
}

TEST_F(NexusLlmRawStreamTest, OversizedBodyEndsWithTooLarge) {
  factory_.AddResponse(kUrl, "0123456789");
  auto stream = MakeStream(/*max_bytes=*/4);
  Open(stream.get());
  NexusLlmRawStream::ReadResult read = Read(stream.get());
  EXPECT_TRUE(read.done);
  EXPECT_EQ(read.error_code, "RESPONSE_TOO_LARGE");
}

// Buffered mode: the guest only ever sees the transform's output, delivered
// with its extra fields on the final read.
TEST_F(NexusLlmRawStreamTest, BufferedModeDeliversOnlyTheTransformOutput) {
  factory_.AddResponse(kUrl, "{\"accessToken\":\"at-secret\"}");
  std::string seen_body;
  int seen_status = 0;
  auto stream = MakeStream(
      1024, base::BindOnce(
                [](std::string* seen_body, int* seen_status, std::string body,
                   int status) {
                  *seen_body = body;
                  *seen_status = status;
                  NexusLlmRawStream::TransformResult result;
                  result.body = "{\"accessToken\":\"[captured]\"}";
                  result.extra.Set("captured", base::DictValue());
                  return result;
                },
                &seen_body, &seen_status));
  Open(stream.get());
  NexusLlmRawStream::ReadResult read = Read(stream.get());
  EXPECT_EQ(seen_body, "{\"accessToken\":\"at-secret\"}");
  EXPECT_EQ(seen_status, 200);
  EXPECT_EQ(read.data, "{\"accessToken\":\"[captured]\"}");
  EXPECT_EQ(read.data.find("at-secret"), std::string::npos);
  EXPECT_TRUE(read.done);
  EXPECT_TRUE(read.extra.FindDict("captured"));
}

TEST_F(NexusLlmRawStreamTest, BufferedModeTransformErrorWithholdsBody) {
  factory_.AddResponse(kUrl, "token=at-secret");
  auto stream = MakeStream(
      1024, base::BindOnce([](std::string, int) {
        NexusLlmRawStream::TransformResult result;
        result.error_code = "CAPTURE_FAILED";
        return result;
      }));
  Open(stream.get());
  NexusLlmRawStream::ReadResult read = Read(stream.get());
  EXPECT_TRUE(read.data.empty());
  EXPECT_TRUE(read.done);
  EXPECT_EQ(read.error_code, "CAPTURE_FAILED");
}

// Redirects are followed; the check decides only whether the secret-bearing
// headers go along.
TEST_F(NexusLlmRawStreamTest, RedirectIsFollowedAndChecked) {
  const GURL target("https://elsewhere.example.net/final");
  net::RedirectInfo redirect;
  redirect.status_code = 307;
  redirect.new_method = "POST";
  redirect.new_url = target;
  network::TestURLLoaderFactory::Redirects redirects;
  redirects.emplace_back(redirect, network::mojom::URLResponseHead::New());
  factory_.AddResponse(GURL(kUrl), network::mojom::URLResponseHead::New(), "",
                       network::URLLoaderCompletionStatus(), std::move(redirects));
  factory_.AddResponse(target.spec(), "moved-body");

  std::vector<GURL> checked;
  auto stream = MakeStream(
      1024, {},
      base::BindRepeating(
          [](std::vector<GURL>* checked, const GURL& url) {
            checked->push_back(url);
            return false;  // off-domain: strip, but follow
          },
          &checked));
  Open(stream.get());
  NexusLlmRawStream::ReadResult read = Read(stream.get());
  ASSERT_EQ(checked.size(), 1u);
  EXPECT_EQ(checked[0], target);
  EXPECT_TRUE(read.error_code.empty()) << read.error_code;
}

TEST_F(NexusLlmRawStreamTest, DestroyingAnswersAHeldRead) {
  // No response registered: the request stays pending.
  auto stream = MakeStream();
  stream->Start(base::DoNothing());
  std::optional<NexusLlmRawStream::ReadResult> result;
  stream->Read(base::Seconds(10), 1024,
               base::BindOnce(
                   [](std::optional<NexusLlmRawStream::ReadResult>* out,
                      NexusLlmRawStream::ReadResult r) { *out = std::move(r); },
                   &result));
  EXPECT_FALSE(result.has_value());
  stream.reset();
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->done);
  EXPECT_EQ(result->error_code, "CANCELLED");
}

TEST_F(NexusLlmRawStreamTest, DestroyingAnswersAPendingOpen) {
  // No response registered: the open waits for a head that never comes. A
  // cancel by a caller-chosen id deletes the stream in this state; the open
  // must still be answered (a dropped bridge callback answers nothing).
  auto stream = MakeStream();
  int calls = 0;
  std::optional<NexusLlmRawStream::OpenResult> result;
  stream->Start(base::BindOnce(
      [](int* calls, std::optional<NexusLlmRawStream::OpenResult>* out,
         NexusLlmRawStream::OpenResult r) {
        ++*calls;
        *out = std::move(r);
      },
      &calls, &result));
  task_environment_.RunUntilIdle();
  EXPECT_FALSE(result.has_value());
  stream.reset();
  task_environment_.RunUntilIdle();
  EXPECT_EQ(calls, 1);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->error_code, "CANCELLED");
}

}  // namespace
}  // namespace nexus
