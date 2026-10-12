// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_NETWORK_NEXUS_HTTP_BYTE_STREAM_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_NETWORK_NEXUS_HTTP_BYTE_STREAM_H_

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "custom_browser/browser/nexus/api/llm/llm_raw_stream_policy.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/simple_url_loader_stream_consumer.h"
#include "url/gurl.h"

namespace net {
struct RedirectInfo;
}  // namespace net

namespace network {
class SharedURLLoaderFactory;
class SimpleURLLoader;
struct ResourceRequest;
}  // namespace network

namespace network::mojom {
class URLResponseHead;
}  // namespace network::mojom

namespace nexus {

// One HTTP request whose response body is queued as opaque bytes for the guest
// to pull — the core of `model.rawStream*` and `network.stream*`. Half-duplex;
// the host never interprets the body. A redirect hop that |redirect_check|
// rejects drops |secret_headers|. A non-null |body_transform| buffers the body
// and only its output reaches the guest. Lifecycle: Start() → `open` once →
// Read() until `done`; deleting the stream cancels it (a pending `open` or
// read gets `CANCELLED`). UI thread.
class NexusHttpByteStream : public network::SimpleURLLoaderStreamConsumer {
 public:
  struct OpenResult {
    OpenResult();
    OpenResult(OpenResult&&);
    OpenResult& operator=(OpenResult&&);
    ~OpenResult();

    // Empty on success.
    std::string error_code;
    std::string error_message;
    int status = 0;
    std::string status_text;
    std::string final_url;
    base::DictValue headers;
  };
  using OpenCallback = base::OnceCallback<void(OpenResult)>;

  struct ReadResult {
    ReadResult();
    ReadResult(ReadResult&&);
    ReadResult& operator=(ReadResult&&);
    ~ReadResult();

    std::string data;
    // True once the response has ended and every byte has been handed out.
    bool done = false;
    // Set when the stream ended abnormally (network error, timeout, size
    // cap, egress refusal). Only reported together with `done`.
    std::string error_code;
    std::string error_message;
    // Buffered mode only: the transform's extra fields, on the `done` read.
    base::DictValue extra;
  };
  using ReadCallback = base::OnceCallback<void(ReadResult)>;

  // Whether a redirect target is still within the allowed scope (the
  // provider's domain; the secret's bound origin).
  using RedirectCheck = base::RepeatingCallback<bool(const GURL&)>;

  struct TransformResult {
    TransformResult();
    TransformResult(TransformResult&&);
    TransformResult& operator=(TransformResult&&);
    ~TransformResult();

    std::string body;
    // Non-empty ends the stream with this error instead of |body|.
    std::string error_code;
    std::string error_message;
    base::DictValue extra;
  };
  // (whole body, HTTP status) -> what the guest receives.
  using BodyTransform =
      base::OnceCallback<TransformResult(std::string body, int status)>;

  // Queue size above which the network read is paused until the guest
  // drains it (the consumer's `resume` closure is held).
  static constexpr size_t kHighWaterBytes = 4 * 1024 * 1024;
  // Upper bound on how long a single Read may be held.
  static constexpr base::TimeDelta kMaxReadHold = base::Seconds(30);
  // Error code of an open or read answered because the stream was deleted.
  static constexpr char kCancelledErrorCode[] = "CANCELLED";
  // Error code of a request the egress policy refused.
  static constexpr char kEgressBlockedErrorCode[] = "EGRESS_BLOCKED";

  NexusHttpByteStream(
      scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
      std::unique_ptr<network::ResourceRequest> request,
      std::string body,
      std::string content_type,
      size_t max_response_bytes,
      base::TimeDelta idle_timeout,
      RedirectCheck redirect_check,
      std::vector<std::string> secret_headers,
      BodyTransform body_transform,
      const net::NetworkTrafficAnnotationTag& traffic_annotation);
  ~NexusHttpByteStream() override;

  NexusHttpByteStream(const NexusHttpByteStream&) = delete;
  NexusHttpByteStream& operator=(const NexusHttpByteStream&) = delete;

  // Run this request under the network egress policy
  // (network_egress_policy.h): the network service refuses a loopback /
  // private peer before the request is written, and the IP the socket
  // connected to is checked at the response head and at every redirect hop
  // (skipped through a proxy, whose address is the socket's peer). A refusal
  // ends the stream with kEgressBlockedErrorCode. Call before Start().
  void EnableEgressPolicy();

  void Start(OpenCallback open);

  // Answers immediately when bytes are queued or the stream has ended;
  // otherwise holds up to |wait| (capped at kMaxReadHold) for either. At
  // most one Read may be outstanding — a second one supersedes the first,
  // which is answered empty.
  void Read(base::TimeDelta wait, size_t max_bytes, ReadCallback callback);

  bool finished() const { return finished_; }

  // network::SimpleURLLoaderStreamConsumer:
  void OnDataReceived(std::string_view data, base::OnceClosure resume) override;
  void OnComplete(bool success) override;
  void OnRetry(base::OnceClosure start_retry) override;

 private:
  void OnResponseStarted(const GURL& final_url,
                         const network::mojom::URLResponseHead& head);
  void OnRedirect(const GURL& url_before_redirect,
                  const net::RedirectInfo& redirect_info,
                  const network::mojom::URLResponseHead& response_head,
                  std::vector<std::string>* removed_headers);
  // The egress refusal for the peer a response came from, or nullopt.
  std::optional<std::string> EgressRefusalForHead(
      const network::mojom::URLResponseHead& head) const;
  void Fail(std::string code, std::string message);
  void FinishOpenWithError(std::string code, std::string message);
  void OnIdleTimeout();
  void AnswerPendingRead();
  void MaybeResume();

  scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory_;
  std::unique_ptr<network::ResourceRequest> request_;
  // The URL as requested, for egress messages (request_ is consumed).
  const GURL request_url_;
  std::string body_;
  std::string content_type_;
  const size_t max_response_bytes_;
  const base::TimeDelta idle_timeout_;
  RedirectCheck redirect_check_;
  const std::vector<std::string> secret_headers_;
  BodyTransform body_transform_;
  const net::NetworkTrafficAnnotationTag traffic_annotation_;
  bool egress_policy_ = false;
  // Buffered mode: the whole body, until OnComplete runs the transform.
  std::string buffered_body_;
  int status_ = 0;
  base::DictValue extra_;

  std::unique_ptr<network::SimpleURLLoader> loader_;
  OpenCallback open_;
  RawStreamChunkQueue queue_;
  size_t total_bytes_ = 0;
  base::OnceClosure resume_;

  // The response ended (normally or not). The stream is `done` for the
  // guest once this is set AND the queue is drained.
  bool finished_ = false;
  std::string error_code_;
  std::string error_message_;

  ReadCallback pending_read_;
  size_t pending_read_max_bytes_ = 0;
  base::OneShotTimer read_hold_timer_;
  base::OneShotTimer idle_timer_;

  base::WeakPtrFactory<NexusHttpByteStream> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_NETWORK_NEXUS_HTTP_BYTE_STREAM_H_
