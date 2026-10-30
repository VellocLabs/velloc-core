// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/llm/nexus_llm_raw_stream.h"

#include <utility>

#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"

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
    : NexusHttpByteStream(std::move(url_loader_factory),
                          std::move(request),
                          std::move(body),
                          std::move(content_type),
                          max_response_bytes,
                          idle_timeout,
                          std::move(redirect_check),
                          std::move(secret_headers),
                          std::move(body_transform),
                          kTrafficAnnotation) {}

NexusLlmRawStream::~NexusLlmRawStream() = default;

}  // namespace nexus
