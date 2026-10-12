// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_NEXUS_LLM_RAW_STREAM_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_NEXUS_LLM_RAW_STREAM_H_

#include <memory>
#include <string>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"
#include "custom_browser/browser/nexus/api/network/nexus_http_byte_stream.h"

namespace network {
class SharedURLLoaderFactory;
struct ResourceRequest;
}  // namespace network

namespace nexus {

// One `model.rawStreamOpen` request: a credentialed HTTP request to a
// provider's domain whose body is queued as opaque bytes for
// `model.rawStreamRead`. Secret substitution and domain anchoring live in
// NexusLlmBridgeAdapter::HandleRawStreamOpenOnUI; this class fixes only the
// traffic annotation.
class NexusLlmRawStream : public NexusHttpByteStream {
 public:
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
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_NEXUS_LLM_RAW_STREAM_H_
