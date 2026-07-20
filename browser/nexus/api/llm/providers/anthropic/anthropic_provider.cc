#include "custom_browser/browser/nexus/api/llm/providers/anthropic/anthropic_provider.h"

#include "custom_browser/browser/nexus/api/llm/providers/anthropic/anthropic_schema_sanitizer.h"
#include "custom_browser/browser/nexus/nexus_secret_crypt.h"

#include <array>
#include <utility>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/bind_post_task.h"
#include "base/task/sequenced_task_runner.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "mojo/public/cpp/system/simple_watcher.h"
#include "net/base/net_errors.h"
#include "net/http/http_request_headers.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/resource_request_body.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/early_hints.mojom.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "url/gurl.h"

namespace nexus {

namespace {

constexpr std::string_view kAnthropicProviderId = "anthropic";
constexpr std::string_view kAnthropicDisplayName = "Anthropic";
constexpr std::string_view kAnthropicApiVersion = "2023-06-01";

constexpr size_t kMaxResponseSizeBytes =
    network::SimpleURLLoader::kMaxBoundedStringDownloadSize;

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("nexus_anthropic_provider", R"(
      semantics {
        sender: "Velloc AI Agent Platform"
        description:
          "Sends prompts to the Anthropic Messages API for AI text "
          "generation as part of the Velloc agent platform."
        trigger:
          "User interacts with the Velloc AI agent panel, which "
          "triggers LLM inference through the configured Anthropic "
          "provider."
        data:
          "User-provided prompts, conversation history, and tool "
          "definitions. No personal data is sent beyond what the user "
          "explicitly provides in the conversation."
        destination: OTHER
        internal {
          contacts {
            email: "niclas@niclas.cc"
          }
        }
        user_data {
          type: OTHER
        }
        last_reviewed: "2026-03-13"
      }
      policy {
        cookies_allowed: NO
        setting:
          "Users configure the Anthropic provider and API key in "
          "Shift Settings > AI Agent > Provider."
        policy_exception_justification:
          "Not yet covered by enterprise policy."
      }
    )");

FinishReason TranslateStopReason(std::string_view stop_reason) {
  if (stop_reason == "end_turn" || stop_reason == "stop_sequence")
    return FinishReason::kStop;
  if (stop_reason == "max_tokens")
    return FinishReason::kLength;
  if (stop_reason == "tool_use")
    return FinishReason::kToolCalls;
  return FinishReason::kOther;
}

std::string_view FinishReasonToString(FinishReason reason) {
  switch (reason) {
    case FinishReason::kStop:
      return "stop";
    case FinishReason::kLength:
      return "length";
    case FinishReason::kToolCalls:
      return "tool-calls";
    case FinishReason::kContentFilter:
      return "content-filter";
    case FinishReason::kError:
      return "error";
    case FinishReason::kOther:
    case FinishReason::kUnknown:
      return "unknown";
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// StreamSession — reads an Anthropic SSE response via URLLoaderClient
// ---------------------------------------------------------------------------

class AnthropicProvider::StreamSession
    : public network::mojom::URLLoaderClient {
 public:
  StreamSession(StreamChunkCallback chunk_callback,
                StreamDoneCallback done_callback,
                base::OnceClosure completion_closure)
      : chunk_callback_(std::move(chunk_callback)),
        done_callback_(std::move(done_callback)),
        completion_closure_(std::move(completion_closure)),
        pipe_watcher_(FROM_HERE,
                      mojo::SimpleWatcher::ArmingPolicy::MANUAL) {}

  ~StreamSession() override {
    pipe_watcher_.Cancel();
    if (done_callback_)
      std::move(done_callback_).Run("Request cancelled");
  }

  void Start(scoped_refptr<network::SharedURLLoaderFactory> factory,
             std::unique_ptr<network::ResourceRequest> request,
             const std::string& body_json) {
    request->request_body = network::ResourceRequestBody::CreateFromCopyOfBytes(
        base::as_byte_span(body_json));

    factory->CreateLoaderAndStart(
        loader_.BindNewPipeAndPassReceiver(),
        /*request_id=*/0,
        /*options=*/0,
        *request,
        receiver_.BindNewPipeAndPassRemote(),
        net::MutableNetworkTrafficAnnotationTag(kTrafficAnnotation));
  }

  // network::mojom::URLLoaderClient:
  void OnReceiveResponse(
      network::mojom::URLResponseHeadPtr head,
      mojo::ScopedDataPipeConsumerHandle body,
      std::optional<mojo_base::BigBuffer> cached_metadata) override {
    response_code_ =
        head->headers ? head->headers->response_code() : 0;

    if (!body.is_valid()) {
      Complete(response_code_ == 200
                   ? std::nullopt
                   : std::make_optional("HTTP " +
                                        base::NumberToString(response_code_)));
      return;
    }

    body_pipe_ = std::move(body);
    pipe_watcher_.Watch(
        body_pipe_.get(),
        MOJO_HANDLE_SIGNAL_READABLE | MOJO_HANDLE_SIGNAL_PEER_CLOSED,
        MOJO_TRIGGER_CONDITION_SIGNALS_SATISFIED,
        base::BindRepeating(&StreamSession::ReadPipe,
                            base::Unretained(this)));
    pipe_watcher_.ArmOrNotify();
  }

  void OnReceiveRedirect(
      const net::RedirectInfo& redirect_info,
      network::mojom::URLResponseHeadPtr head) override {
    Complete("Unexpected redirect from Anthropic API");
  }

  void OnReceiveEarlyHints(
      network::mojom::EarlyHintsPtr early_hints) override {}

  void OnUploadProgress(int64_t current_position,
                        int64_t total_size,
                        OnUploadProgressCallback callback) override {
    std::move(callback).Run();
  }

  void OnTransferSizeUpdated(int32_t transfer_size_diff) override {}

  void OnComplete(
      const network::URLLoaderCompletionStatus& status) override {
    if (completed_)
      return;
    ReadRemainingPipeData();
    // Only parse the buffer as SSE for successful responses. A non-200 body
    // is a JSON error payload — running it through the SSE parser could
    // consume parts of buffer_ (anything containing "\n\n") before the error
    // extraction below reads it.
    if (response_code_ == 200)
      ProcessBuffer();

    if (response_code_ != 200) {
      std::string error =
          "HTTP " + base::NumberToString(response_code_);
      auto parsed =
          base::JSONReader::ReadDict(buffer_, base::JSON_PARSE_RFC);
      if (parsed) {
        if (const auto* err = parsed->FindDict("error")) {
          if (const auto* msg = err->FindString("message"))
            error = *msg;
        }
      } else {
        LOG(ERROR) << "Anthropic API returned HTTP " << response_code_
                   << " with non-JSON body: " << buffer_;
      }
      Complete(std::move(error));
      return;
    }

    if (status.error_code != net::OK) {
      Complete(net::ErrorToString(status.error_code));
      return;
    }

    Complete(std::nullopt);
  }

 private:
  struct ContentBlock {
    std::string type;
    int index = -1;
    std::string tool_call_id;
    std::string tool_name;
    std::string accumulated_json;
  };

  void ReadPipe(MojoResult result, const mojo::HandleSignalsState& state) {
    ReadRemainingPipeData();
    if (response_code_ == 200)
      ProcessBuffer();
    // ProcessBuffer can hit a terminal `error` SSE event, which runs
    // Complete() and cancels the watcher — re-arming a cancelled watcher is
    // invalid, so bail out once completed.
    if (completed_)
      return;
    pipe_watcher_.ArmOrNotify();
  }

  void ReadRemainingPipeData() {
    if (!body_pipe_.is_valid())
      return;
    while (true) {
      std::array<uint8_t, 4096> buf;
      size_t num_bytes = buf.size();
      MojoResult read_result =
          body_pipe_->ReadData(MOJO_READ_DATA_FLAG_NONE, buf, num_bytes);
      if (read_result == MOJO_RESULT_SHOULD_WAIT)
        break;
      if (read_result != MOJO_RESULT_OK)
        break;
      buffer_.append(reinterpret_cast<const char*>(buf.data()), num_bytes);
    }
  }

  void ProcessBuffer() {
    // Stop as soon as a terminal `error` SSE event completes the session —
    // don't keep emitting chunks for a stream that already reported done.
    while (!completed_) {
      size_t pos = buffer_.find("\n\n");
      if (pos == std::string::npos)
        break;

      std::string block_copy(buffer_, 0, pos);
      buffer_.erase(0, pos + 2);

      std::string event_type;
      std::string event_data;
      std::string_view block_view(block_copy);
      size_t line_start = 0;

      while (line_start < block_view.size()) {
        size_t line_end = block_view.find('\n', line_start);
        if (line_end == std::string_view::npos)
          line_end = block_view.size();

        std::string_view line =
            block_view.substr(line_start, line_end - line_start);
        if (!line.empty() && line.back() == '\r')
          line.remove_suffix(1);

        if (line.starts_with("event: ")) {
          event_type = std::string(line.substr(7));
        } else if (line.starts_with("data: ")) {
          if (!event_data.empty())
            event_data += '\n';
          event_data += std::string(line.substr(6));
        }

        line_start = line_end + 1;
      }

      if (!event_data.empty())
        HandleEvent(event_type, event_data);
    }
  }

  void HandleEvent(const std::string& event_type, const std::string& data) {
    auto parsed = base::JSONReader::ReadDict(data, base::JSON_PARSE_RFC);
    if (!parsed) {
      DLOG(ERROR) << "Anthropic SSE: failed to parse data for event '"
                  << event_type << "'";
      return;
    }

    const auto& dict = *parsed;

    if (event_type == "message_start") {
      if (const auto* message = dict.FindDict("message")) {
        if (const auto* usage = message->FindDict("usage")) {
          prompt_tokens_ = usage->FindInt("input_tokens").value_or(0);
          cache_read_input_tokens_ =
              usage->FindInt("cache_read_input_tokens").value_or(0);
          cache_creation_input_tokens_ =
              usage->FindInt("cache_creation_input_tokens").value_or(0);

          // Emit an early usage update so the UI can surface cache hits and
          // prompt token counts before the final finish chunk arrives.
          base::DictValue early_chunk;
          early_chunk.Set("type", "usage-update");
          base::DictValue early_usage;
          early_usage.Set("promptTokens", prompt_tokens_);
          early_usage.Set("completionTokens", 0);
          early_usage.Set("cacheReadInputTokens", cache_read_input_tokens_);
          early_usage.Set("cacheCreationInputTokens",
                          cache_creation_input_tokens_);
          early_chunk.Set("usage", std::move(early_usage));
          chunk_callback_.Run(std::move(early_chunk));
        }
      }

    } else if (event_type == "content_block_start") {
      int index = dict.FindInt("index").value_or(-1);
      const auto* block = dict.FindDict("content_block");
      if (!block || index < 0)
        return;
      const auto* type = block->FindString("type");
      if (!type)
        return;

      ContentBlock cb;
      cb.type = *type;
      cb.index = index;

      if (*type == "tool_use") {
        if (const auto* id = block->FindString("id"))
          cb.tool_call_id = *id;
        if (const auto* name = block->FindString("name"))
          cb.tool_name = *name;

        base::DictValue chunk;
        chunk.Set("type", "tool-call-streaming-start");
        chunk.Set("toolCallId", cb.tool_call_id);
        chunk.Set("toolName", cb.tool_name);
        chunk_callback_.Run(std::move(chunk));
      }

      blocks_[index] = std::move(cb);

    } else if (event_type == "content_block_delta") {
      const auto* delta = dict.FindDict("delta");
      if (!delta)
        return;
      const auto* delta_type = delta->FindString("type");
      if (!delta_type)
        return;

      int index = dict.FindInt("index").value_or(-1);

      if (*delta_type == "text_delta") {
        const auto* text = delta->FindString("text");
        if (text) {
          base::DictValue chunk;
          chunk.Set("type", "text-delta");
          chunk.Set("textDelta", *text);
          chunk_callback_.Run(std::move(chunk));
        }

      } else if (*delta_type == "input_json_delta") {
        const auto* partial = delta->FindString("partial_json");
        if (partial) {
          auto it = blocks_.find(index);
          std::string tool_call_id;
          if (it != blocks_.end()) {
            it->second.accumulated_json += *partial;
            tool_call_id = it->second.tool_call_id;
          }
          base::DictValue chunk;
          chunk.Set("type", "tool-call-delta");
          chunk.Set("toolCallId", tool_call_id);
          chunk.Set("argsTextDelta", *partial);
          chunk_callback_.Run(std::move(chunk));
        }

      } else if (*delta_type == "thinking_delta") {
        const auto* thinking = delta->FindString("thinking");
        if (thinking) {
          base::DictValue chunk;
          chunk.Set("type", "reasoning-delta");
          chunk.Set("textDelta", *thinking);
          chunk_callback_.Run(std::move(chunk));
        }
      }

    } else if (event_type == "content_block_stop") {
      int index = dict.FindInt("index").value_or(-1);
      auto it = blocks_.find(index);
      if (it == blocks_.end())
        return;

      if (it->second.type == "tool_use") {
        base::DictValue chunk;
        chunk.Set("type", "tool-call");
        chunk.Set("toolCallType", "function");
        chunk.Set("toolCallId", it->second.tool_call_id);
        chunk.Set("toolName", it->second.tool_name);
        chunk.Set("args", it->second.accumulated_json);
        chunk_callback_.Run(std::move(chunk));
      }

      blocks_.erase(it);

    } else if (event_type == "message_delta") {
      FinishReason reason = FinishReason::kUnknown;
      if (const auto* delta_obj = dict.FindDict("delta")) {
        if (const auto* sr = delta_obj->FindString("stop_reason"))
          reason = TranslateStopReason(*sr);
      }
      if (const auto* usage = dict.FindDict("usage"))
        completion_tokens_ = usage->FindInt("output_tokens").value_or(0);

      base::DictValue chunk;
      chunk.Set("type", "finish");
      chunk.Set("finishReason", std::string(FinishReasonToString(reason)));

      base::DictValue usage;
      usage.Set("promptTokens", prompt_tokens_);
      usage.Set("completionTokens", completion_tokens_);
      usage.Set("cacheReadInputTokens", cache_read_input_tokens_);
      usage.Set("cacheCreationInputTokens", cache_creation_input_tokens_);
      chunk.Set("usage", std::move(usage));
      chunk_callback_.Run(std::move(chunk));

    } else if (event_type == "error") {
      const auto* err = dict.FindDict("error");
      std::string message = "Unknown API error";
      if (err) {
        if (const auto* msg = err->FindString("message"))
          message = *msg;
      }
      Complete(std::move(message));

    }
    // "message_stop" and "ping" are intentionally ignored here;
    // completion is driven by OnComplete().
  }

  void Complete(std::optional<std::string> error) {
    if (completed_)
      return;
    completed_ = true;
    pipe_watcher_.Cancel();

    if (done_callback_)
      std::move(done_callback_).Run(std::move(error));

    // May destroy |this| — must be the last statement.
    if (completion_closure_)
      std::move(completion_closure_).Run();
  }

  StreamChunkCallback chunk_callback_;
  StreamDoneCallback done_callback_;
  base::OnceClosure completion_closure_;

  mojo::Remote<network::mojom::URLLoader> loader_;
  mojo::Receiver<network::mojom::URLLoaderClient> receiver_{this};
  mojo::ScopedDataPipeConsumerHandle body_pipe_;
  mojo::SimpleWatcher pipe_watcher_;

  std::string buffer_;
  std::map<int, ContentBlock> blocks_;
  int response_code_ = 0;
  int prompt_tokens_ = 0;
  int completion_tokens_ = 0;
  // Cache fields reported on `message_start.message.usage`; Anthropic does
  // not repeat them on `message_delta`, so they're captured once and reused
  // in the emitted finish chunk.
  int cache_read_input_tokens_ = 0;
  int cache_creation_input_tokens_ = 0;
  bool completed_ = false;
};

// ---------------------------------------------------------------------------
// PendingGenerate — tracks an in-flight non-streaming request
// ---------------------------------------------------------------------------

struct AnthropicProvider::PendingGenerate {
  std::unique_ptr<network::SimpleURLLoader> loader;
  GenerateCallback callback;
  base::DictValue request_body;
};

// ---------------------------------------------------------------------------
// AnthropicProvider
// ---------------------------------------------------------------------------

AnthropicProvider::AnthropicProvider(
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory)
    : AnthropicProvider(std::move(url_loader_factory),
                        std::string(kAnthropicProviderId),
                        std::string(kAnthropicDisplayName),
                        "https://api.anthropic.com") {}

AnthropicProvider::AnthropicProvider(
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
    std::string provider_id,
    std::string display_name,
    std::string default_base_url,
    bool requires_api_key)
    : provider_id_(std::move(provider_id)),
      display_name_(std::move(display_name)),
      default_base_url_(std::move(default_base_url)),
      requires_api_key_(requires_api_key),
      base_url_(default_base_url_),
      url_loader_factory_(std::move(url_loader_factory)) {}

AnthropicProvider::~AnthropicProvider() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

std::string AnthropicProvider::GetId() const {
  return provider_id_;
}

std::string AnthropicProvider::GetDisplayName() const {
  return display_name_;
}

LlmProviderType AnthropicProvider::GetProviderType() const {
  return LlmProviderType::kCloud;
}

std::vector<ModelInfo> AnthropicProvider::GetSupportedModels() const {
  return fetched_models_;
}

std::string AnthropicProvider::GetDefaultModelId() const {
  // Default-model selection is owned by the TS layer (nexus_web); the native
  // provider no longer publishes a hard-coded default.
  return std::string();
}

bool AnthropicProvider::SupportsStreaming() const {
  return true;
}

bool AnthropicProvider::SupportsTools() const {
  return true;
}

bool AnthropicProvider::IsConfigured() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // Custom/proxy providers may run keyless (requires_api_key_ == false); they
  // count as configured once a base URL is set.
  return requires_api_key_ ? !api_key_.empty() : !base_url_.empty();
}

bool AnthropicProvider::Configure(const base::DictValue& config) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (const auto* key = config.FindString("api_key"))
    api_key_ = NexusSecretCrypt::GetInstance()->Decrypt(*key);

  if (const auto* model = config.FindString("model");
      model && !model->empty()) {
    model_id_ = *model;
  }

  if (const auto* base_url = config.FindString("base_url")) {
    if (!base_url->empty()) {
      base_url_ = *base_url;
    }
  }

  return IsConfigured();
}

// ---------------------------------------------------------------------------
// DoGenerate — non-streaming request
// ---------------------------------------------------------------------------

void AnthropicProvider::DoGenerate(const base::DictValue& call_options,
                                   GenerateCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (!IsConfigured()) {
    GenerateResult err;
    err.finish_reason = FinishReason::kError;
    std::move(callback).Run(std::move(err), "API key not configured");
    return;
  }

  std::string request_id;
  if (const auto* id = call_options.FindString("requestId"))
    request_id = *id;
  else
    request_id = GenerateRequestId();

  base::DictValue body = BuildRequestBody(call_options, /*stream=*/false);

  std::string body_json;
  base::JSONWriter::Write(body, &body_json);

  auto resource_request = CreateResourceRequest();
  auto loader = network::SimpleURLLoader::Create(std::move(resource_request),
                                                 kTrafficAnnotation);
  loader->AttachStringForUpload(body_json, "application/json");
  loader->SetAllowHttpErrorResults(true);

  auto* loader_ptr = loader.get();
  auto pending = std::make_unique<PendingGenerate>();
  pending->loader = std::move(loader);
  pending->callback = std::move(callback);
  pending->request_body = std::move(body);
  pending_generates_[request_id] = std::move(pending);

  loader_ptr->DownloadToString(
      url_loader_factory_.get(),
      base::BindOnce(&AnthropicProvider::OnGenerateComplete,
                     weak_ptr_factory_.GetWeakPtr(), request_id),
      kMaxResponseSizeBytes);
}

// ---------------------------------------------------------------------------
// DoStream — SSE streaming request
// ---------------------------------------------------------------------------

void AnthropicProvider::DoStream(const base::DictValue& call_options,
                                 StreamChunkCallback chunk_callback,
                                 StreamDoneCallback done_callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (!IsConfigured()) {
    std::move(done_callback).Run("API key not configured");
    return;
  }

  std::string request_id;
  if (const auto* id = call_options.FindString("requestId"))
    request_id = *id;
  else
    request_id = GenerateRequestId();

  base::DictValue body = BuildRequestBody(call_options, /*stream=*/true);

  std::string body_json;
  base::JSONWriter::Write(body, &body_json);

  // All three callbacks are wired through BindPostTask on purpose:
  //  - chunk/done: the host/guest chain behind them can call back into
  //    CancelRequest synchronously; that erases pending_streams_ and
  //    destroys the StreamSession while it is still executing
  //    (ProcessBuffer/HandleEvent) — a use-after-free. Posting breaks the
  //    re-entrancy.
  //  - completion: Complete() runs it from inside ProcessBuffer when a
  //    terminal `error` SSE event arrives; running OnStreamComplete
  //    synchronously would destroy the session mid-ProcessBuffer (the loop
  //    then touches the freed buffer_, and ReadPipe re-arms a destroyed
  //    watcher). Posting defers destruction until the session unwinds; the
  //    `completed_` flag suppresses any further emission in the meantime.
  auto task_runner = base::SequencedTaskRunner::GetCurrentDefault();
  auto session = std::make_unique<StreamSession>(
      base::BindPostTask(task_runner, std::move(chunk_callback)),
      base::BindPostTask(task_runner, std::move(done_callback)),
      base::BindPostTask(
          task_runner,
          base::BindOnce(&AnthropicProvider::OnStreamComplete,
                         weak_ptr_factory_.GetWeakPtr(), request_id)));

  auto resource_request = CreateResourceRequest();
  session->Start(url_loader_factory_, std::move(resource_request), body_json);
  pending_streams_[request_id] = std::move(session);
}

// ---------------------------------------------------------------------------
// CancelRequest
// ---------------------------------------------------------------------------

void AnthropicProvider::CancelRequest(const std::string& request_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  auto gen_it = pending_generates_.find(request_id);
  if (gen_it != pending_generates_.end()) {
    auto pending = std::move(gen_it->second);
    pending_generates_.erase(gen_it);
    GenerateResult result;
    result.finish_reason = FinishReason::kError;
    std::move(pending->callback).Run(std::move(result), "Request cancelled");
    return;
  }

  // Destroying the StreamSession triggers the done_callback via destructor.
  pending_streams_.erase(request_id);
}

// ---------------------------------------------------------------------------
// FetchAvailableModels — GET /v1/models
// ---------------------------------------------------------------------------

void AnthropicProvider::FetchAvailableModels(FetchModelsCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (!IsConfigured()) {
    std::move(callback).Run({});
    return;
  }

  // Coalesce concurrent fetches — replacing models_loader_ mid-flight would
  // silently drop the earlier caller's callback (SimpleURLLoader never runs
  // it on destruction), hanging NexusLlmHost::Catalog()'s barrier.
  if (models_loader_) {
    pending_models_callbacks_.push_back(std::move(callback));
    return;
  }
  callback = base::BindOnce(
      [](base::WeakPtr<AnthropicProvider> self, FetchModelsCallback primary,
         std::vector<ModelInfo> models) {
        std::move(primary).Run(models);
        if (self) {
          std::vector<FetchModelsCallback> waiting =
              std::move(self->pending_models_callbacks_);
          self->pending_models_callbacks_.clear();
          for (FetchModelsCallback& cb : waiting) {
            std::move(cb).Run(models);
          }
        }
      },
      weak_ptr_factory_.GetWeakPtr(), std::move(callback));

  auto request = std::make_unique<network::ResourceRequest>();
  request->url = GURL(base_url_ + "/v1/models");
  request->method = "GET";
  request->headers.SetHeader("x-api-key", api_key_);
  request->headers.SetHeader("anthropic-version", "2023-06-01");
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;

  models_loader_ =
      network::SimpleURLLoader::Create(std::move(request), kTrafficAnnotation);

  models_loader_->DownloadToString(
      url_loader_factory_.get(),
      base::BindOnce(&AnthropicProvider::OnFetchModelsComplete,
                     weak_ptr_factory_.GetWeakPtr(), std::move(callback)),
      kMaxResponseSizeBytes);
}

void AnthropicProvider::OnFetchModelsComplete(
    FetchModelsCallback callback,
    std::optional<std::string> response_body) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  models_loader_.reset();

  // Capability flags (supports_tools, supports_vision), context-window size
  // and filtering/display overrides all live in the TS layer. The native
  // provider passes through whatever the /v1/models endpoint returned.
  if (!response_body) {
    std::move(callback).Run({});
    return;
  }

  // Cache the raw JSON as soon as we have a network-successful body. Any
  // downstream parse failure should still leave the prefs cache populated so
  // the TS layer has something to render and a later code change can revisit
  // parsing without a round trip.
  last_raw_catalog_json_ = *response_body;

  auto parsed =
      base::JSONReader::ReadDict(*response_body, base::JSON_PARSE_RFC);
  if (!parsed) {
    std::move(callback).Run({});
    return;
  }

  const base::ListValue* data = parsed->FindList("data");
  if (!data) {
    std::move(callback).Run({});
    return;
  }

  std::vector<ModelInfo> models;
  for (const auto& model_val : *data) {
    const base::DictValue* model = model_val.GetIfDict();
    if (!model)
      continue;

    const std::string* id = model->FindString("id");
    if (!id || id->empty())
      continue;

    ModelInfo info;
    info.id = *id;
    const std::string* display_name = model->FindString("display_name");
    info.name = display_name ? *display_name : *id;

    models.push_back(std::move(info));
  }

  fetched_models_ = models;
  std::move(callback).Run(std::move(models));
}

void AnthropicProvider::LoadCachedCatalogJson(const std::string& raw_json) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (raw_json.empty())
    return;
  OnFetchModelsComplete(base::DoNothing(), raw_json);
}

std::string AnthropicProvider::GetLastFetchedRawCatalogJson() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return last_raw_catalog_json_;
}

// ---------------------------------------------------------------------------
// Request building
// ---------------------------------------------------------------------------

base::DictValue AnthropicProvider::BuildRequestBody(
    const base::DictValue& call_options,
    bool stream) const {
  base::DictValue body;

  const auto* model = call_options.FindString("model");
  body.Set("model", model ? *model : model_id_);

  std::string system_prompt;
  if (const auto* messages = call_options.FindList("messages")) {
    base::ListValue translated = TranslateMessages(*messages, system_prompt);
    // Drop a third cache breakpoint at the end of the conversation so
    // turns 2..N can also reuse the full message history. Anthropic
    // allows up to 4 breakpoints per request; we use 3 (system, tools,
    // conversation tail) — the 4th is reserved for future granular
    // conversation caching (e.g. an additional breakpoint mid-history
    // for very long sessions). Skip when the conversation is empty.
    if (!translated.empty()) {
      auto* last_msg = translated.back().GetIfDict();
      if (last_msg) {
        if (auto* content = last_msg->FindList("content")) {
          if (!content->empty()) {
            auto* last_part = content->back().GetIfDict();
            if (last_part) {
              base::DictValue cache_control;
              cache_control.Set("type", "ephemeral");
              last_part->Set("cache_control", std::move(cache_control));
            }
          }
        }
      }
    }
    body.Set("messages", std::move(translated));
  }
  if (!system_prompt.empty()) {
    // Emit `system` as a content-block array with cache_control on the
    // single text block. Anthropic prompt cache requires array form +
    // explicit cache_control to register a breakpoint; a plain string
    // (the previous behavior) is never cached. After the nexus_web
    // cleanup the merged system instruction is session-stable
    // (defaultSystemPrompt + locale/timezone/workspace/mcpClients +
    // toolUseContext fields that don't change per turn), so caching it
    // is the highest-ROI optimization available — turns 2..N read the
    // entire system from cache for ~10% of the per-token price.
    base::DictValue text_block;
    text_block.Set("type", "text");
    text_block.Set("text", system_prompt);
    base::DictValue cache_control;
    cache_control.Set("type", "ephemeral");
    text_block.Set("cache_control", std::move(cache_control));
    base::ListValue system_array;
    system_array.Append(std::move(text_block));
    body.Set("system", std::move(system_array));
  }

  // Anthropic requires this field; TS always sends a resolved value.
  body.Set("max_tokens", call_options.FindInt("maxTokens").value_or(8192));

  if (auto temp = call_options.FindDouble("temperature"))
    body.Set("temperature", *temp);
  if (auto top_p = call_options.FindDouble("topP"))
    body.Set("top_p", *top_p);
  if (auto top_k = call_options.FindInt("topK"))
    body.Set("top_k", *top_k);
  if (const auto* stops = call_options.FindList("stopSequences"))
    body.Set("stop_sequences", stops->Clone());

  if (const auto* tools = call_options.FindList("tools")) {
    auto translated = TranslateTools(*tools);
    if (!translated.empty())
      body.Set("tools", std::move(translated));
  }
  if (const auto* choice = call_options.FindDict("toolChoice"))
    body.Set("tool_choice", TranslateToolChoice(*choice));

  // Merge TS-built provider-specific fields last so they override base params.
  if (const auto* ext = call_options.FindDict("providerBodyExtension")) {
    for (auto [key, val] : *ext) {
      body.Set(key, val.Clone());
    }
  }

  body.Set("stream", stream);
  return body;
}

base::ListValue AnthropicProvider::TranslateMessages(
    const base::ListValue& messages,
    std::string& system_prompt) const {
  base::ListValue result;

  for (const auto& msg_val : messages) {
    const auto* msg = msg_val.GetIfDict();
    if (!msg)
      continue;

    const auto* role = msg->FindString("role");
    if (!role)
      continue;

    // System messages → top-level "system" field.
    if (*role == "system") {
      if (const auto* content_str = msg->FindString("content")) {
        if (!system_prompt.empty())
          system_prompt += "\n\n";
        system_prompt += *content_str;
      } else if (const auto* parts = msg->FindList("content")) {
        for (const auto& p : *parts) {
          const auto* pd = p.GetIfDict();
          if (pd) {
            if (const auto* t = pd->FindString("text")) {
              if (!system_prompt.empty())
                system_prompt += "\n\n";
              system_prompt += *t;
            }
          }
        }
      }
      continue;
    }

    std::string anthropic_role = (*role == "tool") ? "user" : *role;

    base::ListValue content_parts;
    if (const auto* content_str = msg->FindString("content")) {
      base::DictValue text_part;
      text_part.Set("type", "text");
      text_part.Set("text", *content_str);
      content_parts.Append(std::move(text_part));
    } else if (const auto* content_list = msg->FindList("content")) {
      content_parts = TranslateContentParts(*content_list, *role);
    }

    // Merge consecutive same-role messages (required by Anthropic when
    // multiple tool results follow an assistant tool_use message).
    if (!result.empty()) {
      auto* last = result.back().GetIfDict();
      if (last) {
        const auto* last_role = last->FindString("role");
        if (last_role && *last_role == anthropic_role) {
          auto* last_content = last->FindList("content");
          if (last_content) {
            for (auto& part : content_parts)
              last_content->Append(std::move(part));
            continue;
          }
        }
      }
    }

    base::DictValue translated;
    translated.Set("role", anthropic_role);
    translated.Set("content", std::move(content_parts));
    result.Append(std::move(translated));
  }

  return result;
}

base::ListValue AnthropicProvider::TranslateContentParts(
    const base::ListValue& parts,
    std::string_view role) const {
  base::ListValue result;

  for (const auto& part_val : parts) {
    const auto* part = part_val.GetIfDict();
    if (!part)
      continue;

    const auto* type = part->FindString("type");
    if (!type)
      continue;

    if (*type == "text") {
      base::DictValue d;
      d.Set("type", "text");
      d.Set("text", part->FindString("text")
                        ? *part->FindString("text")
                        : std::string());
      result.Append(std::move(d));

    } else if (*type == "image") {
      const auto* image_data = part->FindString("image");
      const auto* mime_type = part->FindString("mimeType");

      base::DictValue source;
      if (image_data &&
          (image_data->starts_with("http://") ||
           image_data->starts_with("https://"))) {
        source.Set("type", "url");
        source.Set("url", *image_data);
      } else {
        source.Set("type", "base64");
        source.Set("media_type",
                    mime_type ? *mime_type : std::string("image/png"));
        source.Set("data", image_data ? *image_data : std::string());
      }

      base::DictValue d;
      d.Set("type", "image");
      d.Set("source", std::move(source));
      result.Append(std::move(d));

    } else if (*type == "document") {
      const auto* data = part->FindString("data");
      const auto* mime_type = part->FindString("mimeType");
      base::DictValue source;
      source.Set("type", "base64");
      source.Set("media_type",
                 mime_type ? *mime_type : std::string("application/pdf"));
      source.Set("data", data ? *data : std::string());
      base::DictValue d;
      d.Set("type", "document");
      d.Set("source", std::move(source));
      result.Append(std::move(d));

    } else if (*type == "tool-call") {
      const auto* id = part->FindString("toolCallId");
      const auto* name = part->FindString("toolName");

      base::DictValue d;
      d.Set("type", "tool_use");
      d.Set("id", id ? *id : std::string());
      d.Set("name", name ? *name : std::string());

      // The `args` field can arrive in two shapes depending on which JS
      // code path produced the message:
      //   - As a dict (the canonical shape from
      //     `serializeMessageForBridge` in `query/deps.ts`, lines 454-465 —
      //     `args` is the raw object as the model produced it).
      //   - As a JSON-encoded string (legacy paths that pre-serialize, or
      //     replay from disk where some serializers stringify args).
      // Previously this code only handled the string case (FindString),
      // so the canonical dict shape silently fell through to an empty
      // input{}. That leaked empty tool_use blocks into Anthropic history
      // — the model saw its own past Write/Edit calls with no arguments
      // and retried them ("my content was dropped"), producing the
      // Write-retry loop observed with claude-sonnet-4-6. Handle both
      // shapes; only fall back to empty input{} as a last resort.
      if (const base::Value* args_val = part->Find("args")) {
        if (args_val->is_dict()) {
          d.Set("input", args_val->GetDict().Clone());
        } else if (args_val->is_string()) {
          auto parsed = base::JSONReader::ReadDict(args_val->GetString(),
                                                    base::JSON_PARSE_RFC);
          if (parsed) {
            d.Set("input", std::move(*parsed));
          } else {
            d.Set("input", base::DictValue());
          }
        } else {
          d.Set("input", base::DictValue());
        }
      } else {
        d.Set("input", base::DictValue());
      }
      result.Append(std::move(d));

    } else if (*type == "tool-result") {
      const auto* tool_call_id = part->FindString("toolCallId");

      base::DictValue d;
      d.Set("type", "tool_result");
      d.Set("tool_use_id",
            tool_call_id ? *tool_call_id : std::string());

      // Anthropic's spec accepts `content` as either a bare string OR a
      // list of content blocks. Both are valid; we emit the bare-string
      // form to match Claude Code's wire shape and Anthropic's own SDK
      // examples for text-only tool results. The list-of-blocks form is
      // the canonical shape only when the result is multimodal (mixes
      // text and image blocks) — using it for a single-text payload
      // sends a thin payload over a rich-payload envelope, which
      // empirically didn't help the retry-spiral problem it was added
      // to fix. The real culprit was ambiguous result strings (e.g.
      // FileTool's old "function is executed successfully, file is
      // created and the content is updated too" wording that didn't
      // confirm bytes-on-disk); the fix lives in the per-tool result
      // builders (see process_tool.cc / file_tool.cc), not in this
      // envelope shape. See D7 entry in
      // C:\Users\myyyb\.claude\plans\compare-the-cc-and-staged-reddy.md
      // for the full history of this iteration.
      // Multimodal tool result: when `result` is a list of content blocks
      // (a BrowserScreenshot emits [{type:text},{type:image}]), use
      // Anthropic's list-of-blocks `content` form so the image is sent as
      // a real image block. Anthropic accepts text + image blocks inside
      // tool_result.content. Text-only results keep the bare-string form.
      if (const base::ListValue* result_list = part->FindList("result")) {
        base::ListValue blocks;
        for (const auto& block_val : *result_list) {
          const base::DictValue* block = block_val.GetIfDict();
          if (!block) {
            continue;
          }
          const std::string* btype = block->FindString("type");
          if (!btype) {
            continue;
          }
          if (*btype == "text") {
            base::DictValue tb;
            tb.Set("type", "text");
            tb.Set("text", block->FindString("text")
                               ? *block->FindString("text")
                               : std::string());
            blocks.Append(std::move(tb));
          } else if (*btype == "image") {
            const std::string* img = block->FindString("image");
            const std::string* mime = block->FindString("mimeType");
            base::DictValue img_source;
            if (img && (img->starts_with("http://") ||
                        img->starts_with("https://"))) {
              img_source.Set("type", "url");
              img_source.Set("url", *img);
            } else {
              img_source.Set("type", "base64");
              // Default aligned with the top-level image content-part
              // path (image/png) so the same missing-mime input yields the
              // same media_type regardless of which branch builds it.
              img_source.Set("media_type",
                             mime ? *mime : std::string("image/png"));
              img_source.Set("data", img ? *img : std::string());
            }
            base::DictValue ib;
            ib.Set("type", "image");
            ib.Set("source", std::move(img_source));
            blocks.Append(std::move(ib));
          } else if (*btype == "document") {
            const std::string* data = block->FindString("data");
            const std::string* mime = block->FindString("mimeType");
            base::DictValue doc_source;
            doc_source.Set("type", "base64");
            doc_source.Set("media_type",
                           mime ? *mime : std::string("application/pdf"));
            doc_source.Set("data", data ? *data : std::string());
            base::DictValue db;
            db.Set("type", "document");
            db.Set("source", std::move(doc_source));
            blocks.Append(std::move(db));
          }
        }
        if (blocks.empty()) {
          // No recognized blocks (empty list, or only block types we don't
          // translate). Anthropic 400s on an empty `content` array, so fall
          // back to the bare-string form carrying the serialized original —
          // never emit `content: []`.
          std::string fallback;
          base::JSONWriter::Write(*result_list, &fallback);
          d.Set("content",
                fallback.empty() ? std::string("(no content)") : fallback);
        } else {
          d.Set("content", std::move(blocks));
        }
      } else {
        std::string content_text;
        if (const auto* result_str = part->FindString("result")) {
          content_text = *result_str;
        } else if (const auto* result_val = part->Find("result")) {
          base::JSONWriter::Write(*result_val, &content_text);
        }
        // Keep tool_result content non-empty (absent/empty result) — an
        // empty string is a dubious tool_result payload for Anthropic.
        if (content_text.empty()) {
          content_text = "(no content)";
        }
        d.Set("content", std::move(content_text));
      }

      if (auto is_error = part->FindBool("isError"))
        d.Set("is_error", *is_error);

      result.Append(std::move(d));
    }
  }

  return result;
}

base::ListValue AnthropicProvider::TranslateTools(
    const base::ListValue& tools) const {
  base::ListValue result;

  for (const auto& tool_val : tools) {
    const auto* tool = tool_val.GetIfDict();
    if (!tool)
      continue;

    base::DictValue d;
    if (const auto* name = tool->FindString("name"))
      d.Set("name", *name);
    if (const auto* desc = tool->FindString("description"))
      d.Set("description", *desc);
    if (const auto* params = tool->FindDict("parameters")) {
      // Strict schemas are authored in OpenAI's structured-outputs dialect
      // (nullable-required: type ["T","null"], enum [null, ...]), which the
      // Anthropic API rejects. Derive the Anthropic form (plain optionals,
      // matching claude_code's zod output) mechanically; non-strict schemas
      // already use plain optionals and pass through unchanged.
      const bool is_strict = tool->FindBool("strict").value_or(false);
      d.Set("input_schema", is_strict
                                ? SanitizeStrictSchemaForAnthropic(*params)
                                : params->Clone());
    }

    // Forward `strict: true` when the JS layer has opted into Anthropic
    // Strict tool use (grammar-constrained sampling). When set, the
    // model literally cannot emit a tool_use block whose `input`
    // violates the schema — eliminates the `args:{}` failure mode
    // that haiku-class models hit on `browser_actions`. The JS layer
    // is also responsible for tightening the schema with
    // `additionalProperties: false` before this lands here; the
    // Anthropic API rejects strict tools that lack it.
    if (auto strict = tool->FindBool("strict"))
      d.Set("strict", *strict);

    // Forward optional canonical example calls (Anthropic
    // `input_examples`). Each entry should be a dict that validates
    // against `input_schema`; Anthropic returns 400 on any violation.
    // Used as a softer alternative to `strict: true` — the model isn't
    // grammar-constrained but does see in-context examples that
    // anchor the structurally-tricky shape of `browser_actions`. The
    // JS guest hands this through under the camelCase key
    // `inputExamples`; we rename to snake_case here for Anthropic's
    // wire format.
    if (const auto* examples = tool->FindList("inputExamples"))
      d.Set("input_examples", examples->Clone());

    result.Append(std::move(d));
  }

  // Mark the last tool with cache_control so the entire tools[] array
  // becomes a cacheable prefix block. Tools rarely change within a
  // session, so this is essentially a static block that pairs naturally
  // with the cached system. Anthropic places the breakpoint at the END
  // of the tagged content, so cache_control on the last tool covers all
  // preceding tools too.
  if (!result.empty()) {
    auto* last = result.back().GetIfDict();
    if (last) {
      base::DictValue cache_control;
      cache_control.Set("type", "ephemeral");
      last->Set("cache_control", std::move(cache_control));
    }
  }

  return result;
}

base::DictValue AnthropicProvider::TranslateToolChoice(
    const base::DictValue& choice) const {
  base::DictValue result;
  const auto* type = choice.FindString("type");
  if (!type)
    return result;

  if (*type == "auto") {
    result.Set("type", "auto");
  } else if (*type == "required") {
    result.Set("type", "any");
  } else if (*type == "tool" || *type == "function") {
    result.Set("type", "tool");
    if (const auto* name = choice.FindString("toolName"))
      result.Set("name", *name);
    else if (const auto* fallback_name = choice.FindString("name"))
      result.Set("name", *fallback_name);
  } else if (*type == "none") {
    // Anthropic's Messages API accepts `{"type": "none"}` directly to mean
    // "the model will not be allowed to use tools." See
    // https://platform.claude.com/docs/en/api/messages — tool_choice.type.
    // The previous translation rewrote this to
    // `{type:"auto", disable_parallel_tool_use:true}`, which has different
    // semantics: `auto` still lets the model emit tool_use blocks (it only
    // disables *parallel* tool use), so callers asking to disable tools
    // entirely were silently ignored.
    result.Set("type", "none");
  }

  return result;
}

// ---------------------------------------------------------------------------
// Response parsing
// ---------------------------------------------------------------------------

GenerateResult AnthropicProvider::ParseResponse(
    const base::DictValue& response) const {
  GenerateResult result;

  if (const auto* content = response.FindList("content")) {
    std::string text_acc;
    std::vector<base::DictValue> tool_calls;
    std::vector<base::DictValue> reasoning;

    for (const auto& block_val : *content) {
      const auto* block = block_val.GetIfDict();
      if (!block)
        continue;
      const auto* type = block->FindString("type");
      if (!type)
        continue;

      if (*type == "text") {
        if (const auto* text = block->FindString("text")) {
          if (!text_acc.empty())
            text_acc += '\n';
          text_acc += *text;
        }

      } else if (*type == "tool_use") {
        base::DictValue tc;
        tc.Set("toolCallType", "function");
        if (const auto* id = block->FindString("id"))
          tc.Set("toolCallId", *id);
        if (const auto* name = block->FindString("name"))
          tc.Set("toolName", *name);
        if (const auto* input = block->FindDict("input")) {
          std::string args_json;
          base::JSONWriter::Write(*input, &args_json);
          tc.Set("args", args_json);
        }
        tool_calls.push_back(std::move(tc));

      } else if (*type == "thinking") {
        if (const auto* thinking = block->FindString("thinking")) {
          base::DictValue r;
          r.Set("type", "text");
          r.Set("text", *thinking);
          reasoning.push_back(std::move(r));
        }
      }
    }

    if (!text_acc.empty())
      result.text = std::move(text_acc);
    if (!tool_calls.empty())
      result.tool_calls = std::move(tool_calls);
    if (!reasoning.empty())
      result.reasoning = std::move(reasoning);
  }

  if (const auto* reason = response.FindString("stop_reason"))
    result.finish_reason = TranslateStopReason(*reason);

  if (const auto* usage = response.FindDict("usage")) {
    result.prompt_tokens = usage->FindInt("input_tokens").value_or(0);
    result.completion_tokens = usage->FindInt("output_tokens").value_or(0);
    result.cache_read_input_tokens =
        usage->FindInt("cache_read_input_tokens").value_or(0);
    result.cache_creation_input_tokens =
        usage->FindInt("cache_creation_input_tokens").value_or(0);
  }

  result.raw_response = response.Clone();
  return result;
}

// ---------------------------------------------------------------------------
// Network helpers
// ---------------------------------------------------------------------------

std::unique_ptr<network::ResourceRequest>
AnthropicProvider::CreateResourceRequest() const {
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = GURL(base_url_ + "/v1/messages");
  request->method = "POST";
  request->headers.SetHeader("x-api-key", api_key_);
  request->headers.SetHeader("anthropic-version",
                             std::string(kAnthropicApiVersion));
  request->headers.SetHeader(net::HttpRequestHeaders::kContentType,
                             "application/json");
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  return request;
}

std::string AnthropicProvider::GenerateRequestId() {
  return base::StrCat(
      {"anthropic-", base::NumberToString(next_request_id_++)});
}

std::string AnthropicProvider::GetDefaultBaseUrl() const {
  return default_base_url_;
}

std::string AnthropicProvider::GetBaseUrl() const {
  return base_url_;
}

void AnthropicProvider::OnGenerateComplete(
    const std::string& request_id,
    std::optional<std::string> response_body) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  auto it = pending_generates_.find(request_id);
  if (it == pending_generates_.end())
    return;

  auto pending = std::move(it->second);
  pending_generates_.erase(it);

  if (!response_body) {
    int net_error = pending->loader->NetError();
    GenerateResult err_result;
    err_result.finish_reason = FinishReason::kError;
    std::move(pending->callback)
        .Run(std::move(err_result), net::ErrorToString(net_error));
    return;
  }

  auto parsed =
      base::JSONReader::ReadDict(*response_body, base::JSON_PARSE_RFC);
  if (!parsed) {
    GenerateResult err_result;
    err_result.finish_reason = FinishReason::kError;
    std::move(pending->callback)
        .Run(std::move(err_result), "Failed to parse Anthropic response");
    return;
  }

  const auto& response_dict = *parsed;

  if (const auto* api_error = response_dict.FindDict("error")) {
    GenerateResult err_result;
    err_result.finish_reason = FinishReason::kError;
    err_result.raw_response = response_dict.Clone();
    const auto* message = api_error->FindString("message");
    std::move(pending->callback)
        .Run(std::move(err_result),
             message ? *message : std::string("Unknown API error"));
    return;
  }

  auto result = ParseResponse(response_dict);
  result.raw_call = std::move(pending->request_body);
  std::move(pending->callback).Run(std::move(result), std::nullopt);
}

void AnthropicProvider::OnStreamComplete(const std::string& request_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  pending_streams_.erase(request_id);
}

}  // namespace nexus
