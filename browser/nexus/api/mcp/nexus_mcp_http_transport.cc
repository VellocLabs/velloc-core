// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/mcp/nexus_mcp_http_transport.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/task/sequenced_task_runner.h"
#include "net/base/net_errors.h"
#include "net/http/http_response_headers.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_response_head.mojom.h"

namespace nexus {

namespace {

// Cap on a single JSON-RPC HTTP response body. `DownloadToString` hard-
// DCHECKs if this exceeds `kMaxBoundedStringDownloadSize` (5 MiB), so we
// pin it there. Ample for typical MCP tool results; a future variant could
// switch to the unbounded download + manual cap (as NexusNetworkHost does)
// if servers return larger payloads.
constexpr size_t kMaxResponseBytes = 5 * 1024 * 1024;

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("nexus_mcp_http", R"(
      semantics {
        sender: "Nexus AI Agent Platform"
        description:
          "Sends a JSON-RPC request to a user-configured MCP (Model "
          "Context Protocol) server over Streamable HTTP, on behalf of "
          "the user's AI agent. Runs in the browser process (not subject "
          "to renderer CORS). Authorization headers, if configured, are "
          "injected from native settings and never exposed to the web "
          "guest runtime."
        trigger:
          "The agent connects to or invokes a tool on an MCP server the "
          "user configured in Nexus settings."
        data:
          "JSON-RPC messages (initialize handshake, tools/list, "
          "tools/call with model-supplied arguments). May include "
          "user-configured authorization headers."
        destination: OTHER
        internal {
          contacts {
            email: "niclas@niclas.cc"
          }
        }
        user_data {
          type: OTHER
        }
        last_reviewed: "2026-05-26"
      }
      policy {
        cookies_allowed: NO
        setting:
          "Users add / remove MCP servers in the Nexus settings panel."
        policy_exception_justification:
          "Not yet covered by enterprise policy."
      })");

}  // namespace

NexusMcpHttpTransport::NexusMcpHttpTransport(
    NexusMcpServerConfig config,
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory)
    : config_(std::move(config)),
      url_(config_.url),
      url_loader_factory_(std::move(url_loader_factory)) {}

NexusMcpHttpTransport::~NexusMcpHttpTransport() = default;

void NexusMcpHttpTransport::Start(base::OnceClosure on_ready,
                                  MessageCallback on_message,
                                  ClosedCallback on_closed) {
  on_message_ = std::move(on_message);
  on_closed_ = std::move(on_closed);

  if (!url_loader_factory_ || !url_.is_valid() ||
      !url_.SchemeIsHTTPOrHTTPS()) {
    closed_ = true;
    if (on_closed_) {
      on_closed_.Run("invalid MCP server URL or no URLLoaderFactory");
    }
    return;
  }
  // No pre-connect step for HTTP — the first POST is `initialize`. Signal
  // readiness asynchronously so the host's call stack unwinds first.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
                                                           std::move(on_ready));
}

void NexusMcpHttpTransport::Send(const base::DictValue& message) {
  if (closed_ || !url_loader_factory_) {
    return;
  }
  std::string body;
  if (!base::JSONWriter::Write(message, &body)) {
    LOG(WARNING) << "[mcp] Failed to serialize JSON-RPC for \"" << config_.id
                 << "\"";
    return;
  }

  auto request = std::make_unique<network::ResourceRequest>();
  request->url = url_;
  request->method = "POST";
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  // Native-injected headers (may include Authorization / secrets).
  for (const auto& [name, value] : config_.headers) {
    if (!name.empty()) {
      request->headers.SetHeader(name, value);
    }
  }
  // MCP Streamable HTTP requires the client to accept both response forms.
  request->headers.SetHeader("Accept", "application/json, text/event-stream");
  if (!session_id_.empty()) {
    request->headers.SetHeader("Mcp-Session-Id", session_id_);
  }

  auto loader =
      network::SimpleURLLoader::Create(std::move(request), kTrafficAnnotation);
  loader->SetAllowHttpErrorResults(true);
  loader->AttachStringForUpload(body, "application/json");

  network::SimpleURLLoader* loader_ptr = loader.get();
  in_flight_.push_back(std::move(loader));
  loader_ptr->DownloadToString(
      url_loader_factory_.get(),
      base::BindOnce(&NexusMcpHttpTransport::OnResponse,
                     weak_factory_.GetWeakPtr(), loader_ptr),
      kMaxResponseBytes);
}

void NexusMcpHttpTransport::OnResponse(network::SimpleURLLoader* loader,
                                       std::optional<std::string> body) {
  // Capture status / session id / content type from the response BEFORE
  // erasing the loader (erase destroys it).
  const int net_error = loader->NetError();
  int response_code = 0;
  std::string content_type;
  if (loader->ResponseInfo() && loader->ResponseInfo()->headers) {
    const net::HttpResponseHeaders& headers = *loader->ResponseInfo()->headers;
    response_code = headers.response_code();
    std::string sid;
    if (headers.EnumerateHeader(nullptr, "Mcp-Session-Id", &sid) &&
        !sid.empty()) {
      session_id_ = sid;
    }
    headers.GetMimeType(&content_type);
  }
  EraseLoader(loader);

  if (closed_) {
    return;  // torn-down transport
  }

  // Network-level failure (DNS, TLS, connection refused, …). Without this
  // the handshake silently stalls until the 30s connect timeout; closing
  // with the net error turns "timed out" into the real reason. (HTTP error
  // codes are delivered as a body via SetAllowHttpErrorResults(true), so
  // they show up under `response_code` below, not as a net error.)
  if (net_error != net::OK &&
      net_error != net::ERR_HTTP_RESPONSE_CODE_FAILURE) {
    if (on_closed_) {
      on_closed_.Run(
          base::StrCat({"network error: ", net::ErrorToString(net_error)}));
    }
    closed_ = true;
    return;
  }

  // HTTP error status (401/403 auth, 404 wrong URL, other 4xx/5xx). A
  // compliant MCP server returns tool/protocol errors as 200 + a JSON-RPC
  // `error`, so a non-2xx here is a transport/auth/config problem whose
  // body is usually NOT a JSON-RPC message — which `DispatchJson` /
  // `OnTransportMessage` would silently drop, stalling the handshake until
  // it times out. Fail fast with the status + a body snippet instead.
  if (response_code >= 400) {
    const std::string snippet =
        body && !body->empty() ? body->substr(0, 256) : std::string();
    if (on_closed_) {
      on_closed_.Run(base::StrCat({"HTTP ",
                                   base::NumberToString(response_code),
                                   snippet.empty() ? "" : ": ", snippet}));
    }
    closed_ = true;
    return;
  }

  if (!body.has_value() || body->empty()) {
    return;  // notification ack (202)
  }

  if (base::EqualsCaseInsensitiveASCII(content_type, "text/event-stream")) {
    DispatchSse(*body);
  } else {
    // Default to JSON (application/json or unspecified).
    DispatchJson(*body);
  }
}

void NexusMcpHttpTransport::DispatchSse(const std::string& body) {
  // Minimal SSE parse: events separated by blank lines; concatenate the
  // `data:` field(s) of each event into one JSON payload. Sufficient for
  // JSON-RPC-over-SSE where each event carries one message.
  std::string data;
  auto flush = [&]() {
    if (!data.empty()) {
      DispatchJson(data);
      data.clear();
    }
  };
  for (std::string_view raw_line : base::SplitStringPiece(
           body, "\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL)) {
    if (raw_line.empty()) {
      flush();  // blank line terminates an event
      continue;
    }
    if (base::StartsWith(raw_line, "data:")) {
      std::string_view chunk = raw_line.substr(5);
      if (base::StartsWith(chunk, " ")) {
        chunk = chunk.substr(1);
      }
      data.append(chunk);
    }
    // Other SSE fields (event:, id:, retry:) are ignored.
  }
  flush();
}

void NexusMcpHttpTransport::DispatchJson(const std::string& text) {
  std::optional<base::Value> parsed =
      base::JSONReader::Read(text, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    LOG(WARNING) << "[mcp] Non-JSON HTTP response from \"" << config_.id
                 << "\"";
    return;
  }
  if (on_message_) {
    on_message_.Run(std::move(*parsed).TakeDict());
  }
}

void NexusMcpHttpTransport::EraseLoader(network::SimpleURLLoader* loader) {
  auto it = std::find_if(
      in_flight_.begin(), in_flight_.end(),
      [loader](const std::unique_ptr<network::SimpleURLLoader>& candidate) {
        return candidate.get() == loader;
      });
  if (it != in_flight_.end()) {
    in_flight_.erase(it);
  }
}

void NexusMcpHttpTransport::Close() {
  closed_ = true;
  in_flight_.clear();  // cancels in-flight requests
}

}  // namespace nexus
