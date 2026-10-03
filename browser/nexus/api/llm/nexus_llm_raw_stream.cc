// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/llm/nexus_llm_raw_stream.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/stringprintf.h"
#include "net/base/net_errors.h"
#include "net/http/http_response_headers.h"
#include "net/url_request/redirect_info.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "url/gurl.h"

namespace nexus {

namespace {

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("nexus_llm_raw_stream", R"(
      semantics {
        sender: "Nexus AI Agent Platform"
        description:
          "Sends a model request to an AI provider the user configured, "
          "for providers whose wire protocol is implemented in the Nexus "
          "web runtime rather than natively (e.g. Connect/protobuf "
          "services). The runtime supplies the encoded request; the "
          "browser attaches the provider credential the user saved and "
          "streams the response back unparsed. Requests are restricted "
          "to the provider's configured domain."
        trigger:
          "User sends a message to the AI assistant with such a provider "
          "selected, or the assistant continues a turn (tool results)."
        data:
          "The conversation content and tool results the user's session "
          "sends to the model, and the provider credential."
        destination: OTHER
        internal {
          contacts {
            email: "niclas@niclas.cc"
          }
        }
        user_data {
          type: USER_CONTENT
        }
        last_reviewed: "2026-09-28"
      }
      policy {
        cookies_allowed: NO
        setting:
          "Users choose and configure AI providers in the assistant "
          "settings; no request is sent without a configured provider."
        policy_exception_justification:
          "Not yet covered by enterprise policy."
      }
    )");

}  // namespace

NexusLlmRawStream::OpenResult::OpenResult() = default;
NexusLlmRawStream::OpenResult::OpenResult(OpenResult&&) = default;
NexusLlmRawStream::OpenResult& NexusLlmRawStream::OpenResult::operator=(
    OpenResult&&) = default;
NexusLlmRawStream::OpenResult::~OpenResult() = default;

NexusLlmRawStream::ReadResult::ReadResult() = default;
NexusLlmRawStream::ReadResult::ReadResult(ReadResult&&) = default;
NexusLlmRawStream::ReadResult& NexusLlmRawStream::ReadResult::operator=(
    ReadResult&&) = default;
NexusLlmRawStream::ReadResult::~ReadResult() = default;

NexusLlmRawStream::TransformResult::TransformResult() = default;
NexusLlmRawStream::TransformResult::TransformResult(TransformResult&&) =
    default;
NexusLlmRawStream::TransformResult&
NexusLlmRawStream::TransformResult::operator=(TransformResult&&) = default;
NexusLlmRawStream::TransformResult::~TransformResult() = default;

NexusLlmRawStream::NexusLlmRawStream(
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
    std::unique_ptr<network::ResourceRequest> request,
    std::string body,
    std::string content_type,
    size_t max_response_bytes,
    base::TimeDelta idle_timeout,
    RedirectCheck redirect_check,
    std::vector<std::string> secret_headers,
    BodyTransform body_transform)
    : url_loader_factory_(std::move(url_loader_factory)),
      request_(std::move(request)),
      body_(std::move(body)),
      content_type_(std::move(content_type)),
      max_response_bytes_(max_response_bytes),
      idle_timeout_(idle_timeout),
      redirect_check_(std::move(redirect_check)),
      secret_headers_(std::move(secret_headers)),
      body_transform_(std::move(body_transform)) {}

NexusLlmRawStream::~NexusLlmRawStream() {
  // An open still waiting for its response head (a `rawStreamCancel` that
  // named a caller-chosen id) must be answered too: a BridgeResponseCallback
  // destroyed unfired sends nothing, so the guest's request would hang until
  // its transport timeout. On adapter teardown the callback's bound WeakPtr
  // is already dead and this is a no-op.
  if (open_) {
    FinishOpenWithError(kCancelledErrorCode,
                        "Stream was cancelled before it opened.");
  }
  // A read still held when the owner drops the stream (cancel, adapter
  // teardown) must still be answered — the guest is awaiting it.
  if (pending_read_) {
    ReadResult result;
    result.done = true;
    result.error_code = kCancelledErrorCode;
    result.error_message = "Stream was cancelled.";
    std::move(pending_read_).Run(std::move(result));
  }
}

void NexusLlmRawStream::Start(OpenCallback open) {
  open_ = std::move(open);
  loader_ = network::SimpleURLLoader::Create(std::move(request_),
                                             kTrafficAnnotation);
  // 4xx/5xx bodies carry the provider's error envelope — stream them to the
  // guest like any other body instead of failing the load.
  loader_->SetAllowHttpErrorResults(true);
  loader_->SetOnResponseStartedCallback(base::BindOnce(
      &NexusLlmRawStream::OnResponseStarted, weak_factory_.GetWeakPtr()));
  loader_->SetOnRedirectCallback(base::BindRepeating(
      &NexusLlmRawStream::OnRedirect, weak_factory_.GetWeakPtr()));
  if (!body_.empty()) {
    loader_->AttachStringForUpload(std::move(body_), content_type_);
  }
  idle_timer_.Start(FROM_HERE, idle_timeout_,
                    base::BindOnce(&NexusLlmRawStream::OnIdleTimeout,
                                   weak_factory_.GetWeakPtr()));
  loader_->DownloadAsStream(url_loader_factory_.get(), this);
}

void NexusLlmRawStream::OnResponseStarted(
    const GURL& final_url,
    const network::mojom::URLResponseHead& head) {
  idle_timer_.Reset();
  if (!open_) {
    return;
  }
  OpenResult result;
  result.final_url = final_url.spec();
  if (head.headers) {
    status_ = head.headers->response_code();
    result.status = status_;
    result.status_text = head.headers->GetStatusText();
    size_t iter = 0;
    std::string name;
    std::string value;
    while (head.headers->EnumerateHeaderLines(&iter, &name, &value)) {
      result.headers.Set(name, value);
    }
  }
  std::move(open_).Run(std::move(result));
}

void NexusLlmRawStream::OnRedirect(
    const GURL& /*url_before_redirect*/,
    const net::RedirectInfo& redirect_info,
    const network::mojom::URLResponseHead& /*response_head*/,
    std::vector<std::string>* removed_headers) {
  if (!redirect_check_ || redirect_check_.Run(redirect_info.new_url)) {
    return;
  }
  // Leaving the provider's domain: the hop goes ahead without the secrets.
  removed_headers->insert(removed_headers->end(), secret_headers_.begin(),
                          secret_headers_.end());
}

void NexusLlmRawStream::OnDataReceived(std::string_view data,
                                       base::OnceClosure resume) {
  idle_timer_.Reset();
  total_bytes_ += data.size();
  if (total_bytes_ > max_response_bytes_) {
    Fail("RESPONSE_TOO_LARGE",
         base::StringPrintf("Response exceeded the %zu-byte cap.",
                            max_response_bytes_));
    return;
  }
  if (body_transform_) {
    // Buffered mode: nothing reaches the guest until the transform ran.
    buffered_body_.append(data);
    std::move(resume).Run();
    return;
  }
  queue_.Append(data);
  if (queue_.size() < kHighWaterBytes) {
    std::move(resume).Run();
  } else {
    // Backpressure: the guest is not pulling fast enough.
    resume_ = std::move(resume);
  }
  AnswerPendingRead();
}

void NexusLlmRawStream::OnComplete(bool success) {
  if (finished_) {
    return;
  }
  const int net_error = loader_ ? loader_->NetError() : net::ERR_FAILED;
  if (!success) {
    Fail("NETWORK_ERROR",
         base::StringPrintf("Request failed: %s",
                            net::ErrorToString(net_error).c_str()));
    return;
  }
  if (open_) {
    // A successful load always passes through OnResponseStarted first; this
    // is only reachable if the network layer skipped it.
    Fail("NETWORK_ERROR", "Response ended without headers.");
    return;
  }
  if (body_transform_) {
    TransformResult transformed = std::move(body_transform_)
                                      .Run(std::move(buffered_body_), status_);
    buffered_body_.clear();
    if (!transformed.error_code.empty()) {
      Fail(std::move(transformed.error_code),
           std::move(transformed.error_message));
      return;
    }
    queue_.Append(transformed.body);
    extra_ = std::move(transformed.extra);
  }
  finished_ = true;
  idle_timer_.Stop();
  AnswerPendingRead();
}

void NexusLlmRawStream::OnRetry(base::OnceClosure start_retry) {
  // Retries are never enabled on this loader (a replayed request could
  // double-bill a model call); the guest owns retry policy.
  std::move(start_retry).Run();
}

void NexusLlmRawStream::Read(base::TimeDelta wait,
                             size_t max_bytes,
                             ReadCallback callback) {
  if (pending_read_) {
    std::move(pending_read_).Run(ReadResult());
  }
  read_hold_timer_.Stop();
  pending_read_ = std::move(callback);
  pending_read_max_bytes_ = std::max<size_t>(max_bytes, 1);
  if (!queue_.empty() || finished_ || wait <= base::TimeDelta()) {
    AnswerPendingRead();
    return;
  }
  read_hold_timer_.Start(
      FROM_HERE, std::min(wait, kMaxReadHold),
      base::BindOnce(
          [](base::WeakPtr<NexusLlmRawStream> self) {
            if (self && self->pending_read_) {
              std::move(self->pending_read_).Run(ReadResult());
            }
          },
          weak_factory_.GetWeakPtr()));
}

void NexusLlmRawStream::AnswerPendingRead() {
  if (!pending_read_) {
    return;
  }
  read_hold_timer_.Stop();
  ReadResult result;
  result.data = queue_.Take(pending_read_max_bytes_);
  result.done = finished_ && queue_.empty();
  if (result.done) {
    result.error_code = error_code_;
    result.error_message = error_message_;
    result.extra = std::move(extra_);
  }
  MaybeResume();
  std::move(pending_read_).Run(std::move(result));
}

void NexusLlmRawStream::MaybeResume() {
  if (resume_ && queue_.size() < kHighWaterBytes / 2) {
    std::move(resume_).Run();
  }
}

void NexusLlmRawStream::Fail(std::string code, std::string message) {
  if (finished_) {
    return;
  }
  finished_ = true;
  idle_timer_.Stop();
  resume_.Reset();
  // A buffered body never reaches the guest on failure: it may hold a
  // secret the transform never got to capture.
  buffered_body_.clear();
  body_transform_.Reset();
  // Dropping the loader cancels the request at the network layer.
  loader_.reset();
  if (open_) {
    FinishOpenWithError(std::move(code), std::move(message));
    return;
  }
  error_code_ = std::move(code);
  error_message_ = std::move(message);
  AnswerPendingRead();
}

void NexusLlmRawStream::FinishOpenWithError(std::string code,
                                            std::string message) {
  OpenResult result;
  result.error_code = std::move(code);
  result.error_message = std::move(message);
  std::move(open_).Run(std::move(result));
}

void NexusLlmRawStream::OnIdleTimeout() {
  Fail("TIMEOUT",
       base::StringPrintf("No response data for %d ms.",
                          static_cast<int>(idle_timeout_.InMilliseconds())));
}

}  // namespace nexus
