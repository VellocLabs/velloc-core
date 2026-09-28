// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_NEXUS_LLM_RAW_STREAM_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_NEXUS_LLM_RAW_STREAM_H_

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
#include "services/network/public/cpp/simple_url_loader_stream_consumer.h"

class GURL;

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

// One `model.rawStreamOpen` request: a credentialed HTTP request to a
// provider's domain whose response body is queued as opaque bytes for the
// guest to pull with `model.rawStreamRead`. The host injects the provider's
// credential (see NexusLlmBridgeAdapter::HandleRawStreamOpenOnUI) and never
// interprets the body — framing and decoding (Connect/protobuf, SSE, …) are
// the guest provider adapter's job, so a wire-format change needs no host
// release.
//
// Half-duplex by construction: the request body is sent whole, the response
// streams back. A protocol whose native shape is bidirectional is driven as
// a sequence of such requests by the guest.
//
// Redirects are followed — where the provider sends a request is its call —
// but a hop that |redirect_check| reports as leaving the provider's domain
// drops |secret_headers| (the headers a secret was substituted into), the way
// Fetch drops Authorization on a cross-origin redirect. An open redirect on
// the provider's domain therefore cannot carry the credential elsewhere.
//
// Buffered mode (a non-null |body_transform|): the body is NOT streamed. It
// is held whole and handed to the transform at the end; only the transform's
// output reaches the guest. Used to capture secrets from a token response
// before the guest can see them.
//
// Lifecycle: Start() → `open` fires once, with the response head or an
// error → Read() drains the queue until it reports `done` → the owner
// deletes the stream. Deleting it at any point cancels the network request.
// All methods run on the UI thread.
class NexusLlmRawStream : public network::SimpleURLLoaderStreamConsumer {
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
    // cap). Only reported together with `done`.
    std::string error_code;
    std::string error_message;
    // Buffered mode only: the transform's extra fields, on the `done` read.
    base::DictValue extra;
  };
  using ReadCallback = base::OnceCallback<void(ReadResult)>;

  // Whether a redirect target is still within the provider's domain.
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

  NexusLlmRawStream(
      scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
      std::unique_ptr<network::ResourceRequest> request,
      std::string body,
      std::string content_type,
      size_t max_response_bytes,
      base::TimeDelta idle_timeout,
      RedirectCheck redirect_check,
      std::vector<std::string> secret_headers,
      BodyTransform body_transform);
  ~NexusLlmRawStream() override;

  NexusLlmRawStream(const NexusLlmRawStream&) = delete;
  NexusLlmRawStream& operator=(const NexusLlmRawStream&) = delete;

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
  void Fail(std::string code, std::string message);
  void FinishOpenWithError(std::string code, std::string message);
  void OnIdleTimeout();
  void AnswerPendingRead();
  void MaybeResume();

  scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory_;
  std::unique_ptr<network::ResourceRequest> request_;
  std::string body_;
  std::string content_type_;
  const size_t max_response_bytes_;
  const base::TimeDelta idle_timeout_;
  RedirectCheck redirect_check_;
  const std::vector<std::string> secret_headers_;
  BodyTransform body_transform_;
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

  base::WeakPtrFactory<NexusLlmRawStream> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_NEXUS_LLM_RAW_STREAM_H_
