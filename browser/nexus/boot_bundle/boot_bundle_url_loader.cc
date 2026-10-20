// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/boot_bundle/boot_bundle_url_loader.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/byte_size.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/self_deleting.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "custom_browser/browser/nexus/boot_bundle/boot_bundle_store.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "net/base/load_flags.h"
#include "net/base/net_errors.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_util.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/http_request_headers_update_params.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/self_deleting_url_loader_factory.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/cpp/url_loader_factory_builder.h"
#include "services/network/public/mojom/early_hints.mojom.h"
#include "services/network/public/mojom/fetch_api.mojom.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "url/origin.h"

namespace nexus {

namespace {

// Any of these asks for the network or for the cache alone; such a request
// is never answered from the bundle. force-cache (skip validation) is not
// among them: a bundled file is content-addressed, so the bundle is as good
// as a cache entry.
constexpr int kNoBundleFlags =
    net::LOAD_VALIDATE_CACHE | net::LOAD_BYPASS_CACHE |
    net::LOAD_ONLY_FROM_CACHE | net::LOAD_DISABLE_CACHE;

constexpr int kCacheOnlyFlags =
    net::LOAD_ONLY_FROM_CACHE | net::LOAD_SKIP_CACHE_VALIDATION;

bool IsTextType(const std::string& mime_type) {
  return base::StartsWith(mime_type, "text/") ||
         mime_type == "application/json" ||
         mime_type == "application/javascript";
}

network::mojom::URLResponseHeadPtr MakeBundleHead(
    const network::ResourceRequest& request,
    const BootBundleEntry& entry,
    base::Time build_time) {
  const bool text = IsTextType(entry.mime_type);
  std::string raw = base::StrCat(
      {"HTTP/1.1 200 OK\nContent-Type: ", entry.mime_type,
       text ? "; charset=utf-8" : "",
       "\nContent-Length: ", base::NumberToString(entry.size),
       "\nAccess-Control-Allow-Origin: *",
       "\nContent-Security-Policy: frame-ancestors 'none'\n\n"});
  auto head = network::mojom::URLResponseHead::New();
  head->headers = base::MakeRefCounted<net::HttpResponseHeaders>(
      net::HttpUtil::AssembleRawHeaders(raw));
  head->mime_type = entry.mime_type;
  if (text) {
    head->charset = "utf-8";
  }
  head->content_length = static_cast<int64_t>(entry.size);
  // A fixed time keeps the V8 code cache produced for this copy valid on
  // every boot that serves it.
  head->request_time = build_time;
  head->response_time = build_time;
  const bool cross_origin =
      request.request_initiator &&
      !request.request_initiator->IsSameOriginWith(request.url);
  head->response_type = cross_origin
                            ? network::mojom::FetchResponseType::kCors
                            : network::mojom::FetchResponseType::kBasic;
  return head;
}

// Serves one request: the HTTP cache first, then the bundle, then the
// network. Owned by its URLLoader pipe.
class BootBundleURLLoader : public network::mojom::URLLoader,
                            public network::mojom::URLLoaderClient {
 public:
  BootBundleURLLoader(
      const BootBundleStore* store,
      int32_t request_id,
      uint32_t options,
      const network::ResourceRequest& request,
      mojo::PendingRemote<network::mojom::URLLoaderClient> client,
      const net::MutableNetworkTrafficAnnotationTag& traffic_annotation,
      mojo::PendingRemote<network::mojom::URLLoaderFactory> target)
      : store_(store),
        request_id_(request_id),
        options_(options),
        request_(request),
        client_(std::move(client)),
        traffic_annotation_(traffic_annotation),
        target_(std::move(target)) {
    network::ResourceRequest cache_only = request_;
    cache_only.load_flags |= kCacheOnlyFlags;
    Start(cache_only);
  }

  BootBundleURLLoader(const BootBundleURLLoader&) = delete;
  BootBundleURLLoader& operator=(const BootBundleURLLoader&) = delete;
  ~BootBundleURLLoader() override = default;

  // network::mojom::URLLoader:
  void FollowRedirect(
      network::HttpRequestHeadersUpdateParams headers_update_params,
      const std::optional<GURL>& new_url) override {
    if (target_loader_) {
      target_loader_->FollowRedirect(std::move(headers_update_params),
                                     new_url);
    }
  }

  void SetPriority(net::RequestPriority priority,
                   int32_t intra_priority_value) override {
    if (target_loader_) {
      target_loader_->SetPriority(priority, intra_priority_value);
    }
  }

  // network::mojom::URLLoaderClient:
  void OnReceiveEarlyHints(network::mojom::EarlyHintsPtr hints) override {
    client_->OnReceiveEarlyHints(std::move(hints));
  }

  void OnReceiveResponse(
      network::mojom::URLResponseHeadPtr head,
      mojo::ScopedDataPipeConsumerHandle body,
      std::optional<mojo_base::BigBuffer> cached_metadata) override {
    responded_ = true;
    client_->OnReceiveResponse(std::move(head), std::move(body),
                               std::move(cached_metadata));
  }

  void OnReceiveRedirect(const net::RedirectInfo& redirect_info,
                         network::mojom::URLResponseHeadPtr head) override {
    responded_ = true;
    client_->OnReceiveRedirect(redirect_info, std::move(head));
  }

  void OnUploadProgress(int64_t current_position,
                        int64_t total_size,
                        base::OnceCallback<void()> callback) override {
    client_->OnUploadProgress(current_position, total_size,
                              std::move(callback));
  }

  void OnTransferSizeUpdated(int32_t transfer_size_diff) override {
    client_->OnTransferSizeUpdated(transfer_size_diff);
  }

  void OnComplete(const network::URLLoaderCompletionStatus& status) override {
    if (cache_attempt_ && !responded_ &&
        status.error_code == net::ERR_CACHE_MISS) {
      cache_attempt_ = false;
      Detach();
      ServeFromBundle();
      return;
    }
    Detach();
    client_->OnComplete(status);
  }

 private:
  void Start(const network::ResourceRequest& request) {
    target_->CreateLoaderAndStart(
        target_loader_.BindNewPipeAndPassReceiver(), request_id_, options_,
        request, client_receiver_.BindNewPipeAndPassRemote(),
        traffic_annotation_);
    client_receiver_.set_disconnect_handler(
        base::BindOnce(&BootBundleURLLoader::OnTargetDisconnected,
                       base::Unretained(this)));
  }

  void Detach() {
    target_loader_.reset();
    client_receiver_.reset();
  }

  void OnTargetDisconnected() {
    Detach();
    client_->OnComplete(network::URLLoaderCompletionStatus(net::ERR_ABORTED));
  }

  void ServeFromBundle() {
    const BootBundleIndex* index = store_->index();
    const BootBundleEntry* entry =
        index ? index->Lookup(request_.url) : nullptr;
    if (!entry) {
      Start(request_);
      return;
    }
    store_->Read(*entry, base::BindOnce(&BootBundleURLLoader::OnBundleRead,
                                        weak_factory_.GetWeakPtr(), *entry,
                                        index->build_time()));
  }

  void OnBundleRead(BootBundleEntry entry,
                    base::Time build_time,
                    std::optional<std::string> bytes) {
    if (!bytes) {
      Start(request_);
      return;
    }
    const MojoCreateDataPipeOptions pipe_options{
        sizeof(MojoCreateDataPipeOptions), MOJO_CREATE_DATA_PIPE_FLAG_NONE, 1,
        static_cast<uint32_t>(std::max<size_t>(bytes->size(), 1))};
    mojo::ScopedDataPipeProducerHandle producer;
    mojo::ScopedDataPipeConsumerHandle consumer;
    if (mojo::CreateDataPipe(&pipe_options, producer, consumer) !=
            MOJO_RESULT_OK ||
        (!bytes->empty() &&
         producer->WriteAllData(base::as_byte_span(*bytes)) !=
             MOJO_RESULT_OK)) {
      Start(request_);
      return;
    }
    producer.reset();
    client_->OnReceiveResponse(MakeBundleHead(request_, entry, build_time),
                               std::move(consumer), std::nullopt);
    network::URLLoaderCompletionStatus status(net::OK);
    status.decoded_body_length = base::ByteSize(bytes->size());
    status.encoded_body_length = status.decoded_body_length;
    client_->OnComplete(status);
  }

  raw_ptr<const BootBundleStore> store_;
  const int32_t request_id_;
  const uint32_t options_;
  const network::ResourceRequest request_;
  mojo::Remote<network::mojom::URLLoaderClient> client_;
  const net::MutableNetworkTrafficAnnotationTag traffic_annotation_;
  mojo::Remote<network::mojom::URLLoaderFactory> target_;
  mojo::Remote<network::mojom::URLLoader> target_loader_;
  mojo::Receiver<network::mojom::URLLoaderClient> client_receiver_{this};
  bool cache_attempt_ = true;
  bool responded_ = false;
  base::WeakPtrFactory<BootBundleURLLoader> weak_factory_{this};
};

class BootBundleURLLoaderFactory
    : public network::SelfDeletingURLLoaderFactory {
 public:
  BootBundleURLLoaderFactory(
      const BootBundleStore* store,
      mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver,
      mojo::PendingRemote<network::mojom::URLLoaderFactory> target,
      base::SelfDeletingPassKey key)
      : network::SelfDeletingURLLoaderFactory(std::move(receiver), key),
        store_(store),
        target_(std::move(target)) {
    target_.set_disconnect_handler(
        base::BindOnce(&BootBundleURLLoaderFactory::OnTargetDisconnected,
                       base::Unretained(this)));
  }

  // network::mojom::URLLoaderFactory:
  void CreateLoaderAndStart(
      mojo::PendingReceiver<network::mojom::URLLoader> loader,
      int32_t request_id,
      uint32_t options,
      const network::ResourceRequest& request,
      mojo::PendingRemote<network::mojom::URLLoaderClient> client,
      const net::MutableNetworkTrafficAnnotationTag& traffic_annotation)
      override {
    if (!ShouldTryBootBundle(*store_, request)) {
      target_->CreateLoaderAndStart(std::move(loader), request_id, options,
                                    request, std::move(client),
                                    traffic_annotation);
      return;
    }
    // Each loader owns a clone of the target: it may outlive this factory.
    mojo::PendingRemote<network::mojom::URLLoaderFactory> target;
    target_->Clone(target.InitWithNewPipeAndPassReceiver());
    mojo::MakeSelfOwnedReceiver(
        std::make_unique<BootBundleURLLoader>(
            store_, request_id, options, request, std::move(client),
            traffic_annotation, std::move(target)),
        std::move(loader));
  }

 private:
  ~BootBundleURLLoaderFactory() override = default;

  void OnTargetDisconnected() { DisconnectReceiversAndDestroy(); }

  raw_ptr<const BootBundleStore> store_;
  mojo::Remote<network::mojom::URLLoaderFactory> target_;
};

}  // namespace

bool ShouldTryBootBundle(const BootBundleStore& store,
                         const network::ResourceRequest& request) {
  const BootBundleIndex* index = store.index();
  return index && request.method == net::HttpRequestHeaders::kGetMethod &&
         (request.load_flags & kNoBundleFlags) == 0 &&
         index->Lookup(request.url) != nullptr;
}

void CreateBootBundleURLLoaderFactory(
    const BootBundleStore* store,
    mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver,
    mojo::PendingRemote<network::mojom::URLLoaderFactory> target) {
  base::MakeSelfDeleting<BootBundleURLLoaderFactory>(store, std::move(receiver),
                                                    std::move(target));
}

void MaybeProxyURLLoaderFactoryForBootBundle(
    content::BrowserContext* browser_context,
    content::RenderFrameHost* frame,
    bool is_navigation,
    network::URLLoaderFactoryBuilder& builder) {
  const BootBundleStore* store = BootBundleStore::Get();
  if (!store || !store->index()) {
    return;
  }
  auto [receiver, target] = builder.Append();
  CreateBootBundleURLLoaderFactory(store, std::move(receiver),
                                   std::move(target));
}

}  // namespace nexus
