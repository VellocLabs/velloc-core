// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/network/nexus_http_byte_stream.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/stringprintf.h"
#include "custom_browser/browser/nexus/api/network/network_egress_policy.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_errors.h"
#include "net/http/http_response_headers.h"
#include "net/url_request/redirect_info.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "url/gurl.h"

namespace nexus {

NexusHttpByteStream::OpenResult::OpenResult() = default;
NexusHttpByteStream::OpenResult::OpenResult(OpenResult&&) = default;
NexusHttpByteStream::OpenResult& NexusHttpByteStream::OpenResult::operator=(
    OpenResult&&) = default;
NexusHttpByteStream::OpenResult::~OpenResult() = default;

NexusHttpByteStream::ReadResult::ReadResult() = default;
NexusHttpByteStream::ReadResult::ReadResult(ReadResult&&) = default;
NexusHttpByteStream::ReadResult& NexusHttpByteStream::ReadResult::operator=(
    ReadResult&&) = default;
NexusHttpByteStream::ReadResult::~ReadResult() = default;

NexusHttpByteStream::TransformResult::TransformResult() = default;
NexusHttpByteStream::TransformResult::TransformResult(TransformResult&&) =
    default;
NexusHttpByteStream::TransformResult&
NexusHttpByteStream::TransformResult::operator=(TransformResult&&) = default;
NexusHttpByteStream::TransformResult::~TransformResult() = default;

NexusHttpByteStream::NexusHttpByteStream(
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
    std::unique_ptr<network::ResourceRequest> request,
    std::string body,
    std::string content_type,
    size_t max_response_bytes,
    base::TimeDelta idle_timeout,
    RedirectCheck redirect_check,
    std::vector<std::string> secret_headers,
    BodyTransform body_transform,
    const net::NetworkTrafficAnnotationTag& traffic_annotation)
    : url_loader_factory_(std::move(url_loader_factory)),
      request_(std::move(request)),
      request_url_(request_ ? request_->url : GURL()),
      body_(std::move(body)),
      content_type_(std::move(content_type)),
      max_response_bytes_(max_response_bytes),
      idle_timeout_(idle_timeout),
      redirect_check_(std::move(redirect_check)),
      secret_headers_(std::move(secret_headers)),
      body_transform_(std::move(body_transform)),
      traffic_annotation_(traffic_annotation) {}

NexusHttpByteStream::~NexusHttpByteStream() {
  // An open still waiting for its response head (a cancel that named a
  // caller-chosen id) must be answered too: a BridgeResponseCallback
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

void NexusHttpByteStream::EnableEgressPolicy() {
  egress_policy_ = true;
}

void NexusHttpByteStream::Start(OpenCallback open) {
  open_ = std::move(open);
  loader_ = network::SimpleURLLoader::Create(std::move(request_),
                                             traffic_annotation_);
  // 4xx/5xx bodies carry the server's error envelope — stream them to the
  // guest like any other body instead of failing the load.
  loader_->SetAllowHttpErrorResults(true);
  if (egress_policy_) {
    // Pre-send enforcement: the network service refuses a loopback / private
    // peer before any byte of the request is written (see
    // network_egress_policy.h). The head / redirect checks below cover the
    // ranges Chromium counts as public (carrier-grade NAT, multicast).
    loader_->SetURLLoaderFactoryOptions(
        network::mojom::kURLLoadOptionBlockLocalRequest);
  }
  loader_->SetOnResponseStartedCallback(base::BindOnce(
      &NexusHttpByteStream::OnResponseStarted, weak_factory_.GetWeakPtr()));
  loader_->SetOnRedirectCallback(base::BindRepeating(
      &NexusHttpByteStream::OnRedirect, weak_factory_.GetWeakPtr()));
  if (!body_.empty()) {
    loader_->AttachStringForUpload(std::move(body_), content_type_);
  }
  idle_timer_.Start(FROM_HERE, idle_timeout_,
                    base::BindOnce(&NexusHttpByteStream::OnIdleTimeout,
                                   weak_factory_.GetWeakPtr()));
  loader_->DownloadAsStream(url_loader_factory_.get(), this);
}

std::optional<std::string> NexusHttpByteStream::EgressRefusalForHead(
    const network::mojom::URLResponseHead& head) const {
  if (!egress_policy_) {
    return std::nullopt;
  }
  // Through a proxy the socket's peer is the proxy (often a local one), and
  // the proxy resolves the origin's name: there is no origin IP to judge.
  if (!head.proxy_chain.is_direct()) {
    return std::nullopt;
  }
  const net::IPAddress& address = head.remote_endpoint.address();
  std::optional<std::string> refusal = EgressRefusalFor(address);
  if (!refusal) {
    return std::nullopt;
  }
  return base::StringPrintf("%s resolved to %s, a %s",
                            std::string(request_url_.host()).c_str(),
                            address.ToString().c_str(), refusal->c_str());
}

void NexusHttpByteStream::OnResponseStarted(
    const GURL& final_url,
    const network::mojom::URLResponseHead& head) {
  idle_timer_.Reset();
  if (std::optional<std::string> refusal = EgressRefusalForHead(head)) {
    Fail(kEgressBlockedErrorCode,
         "Refused by the egress policy: " + *refusal + ".");
    return;
  }
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

void NexusHttpByteStream::OnRedirect(
    const GURL& /*url_before_redirect*/,
    const net::RedirectInfo& redirect_info,
    const network::mojom::URLResponseHead& response_head,
    std::vector<std::string>* removed_headers) {
  if (egress_policy_) {
    std::optional<std::string> refusal = EgressRefusalForHead(response_head);
    if (!refusal) {
      refusal = EgressRefusalForLiteralHost(redirect_info.new_url);
    }
    if (refusal) {
      // Deleting the loader from inside its own callback is supported; the
      // hop is never dialed.
      Fail(kEgressBlockedErrorCode,
           "Refused by the egress policy: " + *refusal + ".");
      return;
    }
  }
  if (!redirect_check_ || redirect_check_.Run(redirect_info.new_url)) {
    return;
  }
  // Leaving the allowed scope: the hop goes ahead without the secrets.
  removed_headers->insert(removed_headers->end(), secret_headers_.begin(),
                          secret_headers_.end());
}

void NexusHttpByteStream::OnDataReceived(std::string_view data,
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

void NexusHttpByteStream::OnComplete(bool success) {
  if (finished_) {
    return;
  }
  const int net_error = loader_ ? loader_->NetError() : net::ERR_FAILED;
  if (!success) {
    if (egress_policy_) {
      if (std::optional<std::string> refusal =
              EgressRefusalForNetError(net_error, request_url_)) {
        Fail(kEgressBlockedErrorCode,
             "Refused by the egress policy: " + *refusal + ".");
        return;
      }
    }
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

void NexusHttpByteStream::OnRetry(base::OnceClosure start_retry) {
  // Retries are never enabled on this loader (a replayed request could
  // double-bill a model call or repeat a side effect); the guest owns retry
  // policy.
  std::move(start_retry).Run();
}

void NexusHttpByteStream::Read(base::TimeDelta wait,
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
          [](base::WeakPtr<NexusHttpByteStream> self) {
            if (self && self->pending_read_) {
              std::move(self->pending_read_).Run(ReadResult());
            }
          },
          weak_factory_.GetWeakPtr()));
}

void NexusHttpByteStream::AnswerPendingRead() {
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

void NexusHttpByteStream::MaybeResume() {
  if (resume_ && queue_.size() < kHighWaterBytes / 2) {
    std::move(resume_).Run();
  }
}

void NexusHttpByteStream::Fail(std::string code, std::string message) {
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

void NexusHttpByteStream::FinishOpenWithError(std::string code,
                                              std::string message) {
  OpenResult result;
  result.error_code = std::move(code);
  result.error_message = std::move(message);
  std::move(open_).Run(std::move(result));
}

void NexusHttpByteStream::OnIdleTimeout() {
  Fail("TIMEOUT",
       base::StringPrintf("No response data for %d ms.",
                          static_cast<int>(idle_timeout_.InMilliseconds())));
}

}  // namespace nexus
