#include "custom_browser/browser/nexus/api/llm/providers/ollama/ollama_provider.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/bind_post_task.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "base/unguessable_token.h"
#include "custom_browser/browser/nexus/api/llm/llm_error_extract.h"
#include "custom_browser/browser/nexus/api/llm/providers/openai/openai_reasoning.h"
#include "custom_browser/browser/nexus/api/llm/providers/openai/openai_tool_result_messages.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/cpp/simple_url_loader_stream_consumer.h"
#include "url/gurl.h"

namespace nexus {

namespace {

constexpr int kModelFetchTimeoutSeconds = 10;

// Upper bound for non-streaming completion bodies and the /api/tags model
// catalogue. Replaces DownloadToStringOfUnboundedSizeUntilCrashAndDie — a
// misbehaving (or merely mis-addressed) server must not be able to OOM the
// browser process.
constexpr size_t kMaxResponseSizeBytes =
    network::SimpleURLLoader::kMaxBoundedStringDownloadSize;

const net::NetworkTrafficAnnotationTag kOllamaTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("nexus_ollama_provider", R"(
        semantics {
          sender: "Nexus AI Agent Platform"
          description:
            "Sends chat completion requests to a locally running Ollama "
            "server for AI agent functionality."
          trigger:
            "User initiates an AI agent action that requires LLM inference."
          data:
            "Chat messages, model configuration, and tool definitions."
          destination: LOCAL
        }
        policy {
          cookies_allowed: NO
        setting:
            "Users can configure the Ollama server address in Nexus "
            "settings."
        })");

std::string ResolveRequestId(const base::DictValue& call_options) {
  if (const auto* request_id = call_options.FindString("requestId");
      request_id && !request_id->empty()) {
    return *request_id;
  }
  return base::UnguessableToken::Create().ToString();
}

base::DictValue BuildUsageDict(const base::DictValue& usage) {
  base::DictValue out;
  const int prompt_tokens = usage.FindInt("prompt_tokens").value_or(0);
  out.Set("completionTokens", usage.FindInt("completion_tokens").value_or(0));
  // OpenAI-compatible `prompt_tokens` is INCLUSIVE of cached tokens, with
  // `prompt_tokens_details.cached_tokens` a subset. The bridge follows the
  // Anthropic convention where `input_tokens` EXCLUDES cache (the JS token
  // counter sums input + cache_read + cache_creation + output), so subtract
  // the cached portion here to avoid double-counting.
  int cached = 0;
  if (const base::DictValue* details =
          usage.FindDict("prompt_tokens_details")) {
    cached = details->FindInt("cached_tokens").value_or(0);
  }
  if (cached > 0) {
    out.Set("cacheReadInputTokens", cached);
  }
  out.Set("promptTokens", prompt_tokens > cached ? prompt_tokens - cached : 0);
  return out;
}

void MaybeEmitFinish(OllamaProvider::StreamContext& ctx) {
  if (ctx.finish_emitted || !ctx.pending_finish_reason.has_value() ||
      !ctx.pending_usage.has_value()) {
    return;
  }
  base::DictValue chunk;
  chunk.Set("type", "finish");
  chunk.Set("finishReason", *ctx.pending_finish_reason);
  chunk.Set("usage", std::move(*ctx.pending_usage));
  ctx.pending_usage.reset();
  ctx.finish_emitted = true;
  ctx.chunk_callback.Run(std::move(chunk));
}

void EmitStreamEvents(OllamaProvider::StreamContext& ctx,
                      const base::DictValue& chunk) {
  const auto* choices = chunk.FindList("choices");

  // Usage-only chunk (OpenAI-compatible backends with
  // stream_options.include_usage=true).
  if (!choices || choices->empty()) {
    if (const base::DictValue* usage = chunk.FindDict("usage")) {
      ctx.pending_usage = BuildUsageDict(*usage);
      MaybeEmitFinish(ctx);
    }
    return;
  }

  const auto& first_choice = (*choices)[0];
  if (!first_choice.is_dict()) {
    return;
  }
  const auto& choice = first_choice.GetDict();

  const auto* delta = choice.FindDict("delta");
  if (delta) {
    // Thinking models (qwen3, deepseek-r1, gpt-oss) stream their reasoning in
    // `delta.reasoning` on Ollama's OpenAI-compatible endpoint.
    if (std::string thought = ExtractChatCompletionReasoningText(*delta);
        !thought.empty()) {
      base::DictValue reasoning_event;
      reasoning_event.Set("type", "reasoning-delta");
      reasoning_event.Set("textDelta", std::move(thought));
      ctx.chunk_callback.Run(std::move(reasoning_event));
    }

    const auto* content = delta->FindString("content");
    if (content && !content->empty()) {
      base::DictValue text_event;
      text_event.Set("type", "text-delta");
      text_event.Set("textDelta", *content);
      ctx.chunk_callback.Run(std::move(text_event));
    }

    const auto* tool_calls = delta->FindList("tool_calls");
    if (tool_calls) {
      for (const auto& tc_val : *tool_calls) {
        if (!tc_val.is_dict()) {
          continue;
        }
        const auto& tc = tc_val.GetDict();

        // OpenAI-compatible streams key subsequent argument chunks by
        // `index`. The first chunk carries `id` (+ `function.name`); later
        // chunks carry only `index` + `function.arguments`. Mirror the
        // OpenAI provider's two-phase emission: one `tool-call-streaming-
        // start` when we first see the id, then `tool-call-delta` frames
        // with the resolved id for every argument chunk.
        int index = tc.FindInt("index").value_or(0);

        const auto* id = tc.FindString("id");
        if (id && !id->empty()) {
          ctx.tool_call_ids[index] = *id;

          base::DictValue start_event;
          start_event.Set("type", "tool-call-streaming-start");
          start_event.Set("toolCallId", *id);
          start_event.Set("toolCallType", "function");
          if (const auto* function = tc.FindDict("function")) {
            if (const auto* name = function->FindString("name")) {
              start_event.Set("toolName", *name);
            }
          }
          ctx.chunk_callback.Run(std::move(start_event));
        }

        if (const auto* function = tc.FindDict("function")) {
          const auto* args = function->FindString("arguments");
          if (args && !args->empty()) {
            base::DictValue delta_event;
            delta_event.Set("type", "tool-call-delta");
            auto id_it = ctx.tool_call_ids.find(index);
            if (id_it != ctx.tool_call_ids.end()) {
              delta_event.Set("toolCallId", id_it->second);
            }
            delta_event.Set("argsTextDelta", *args);
            ctx.chunk_callback.Run(std::move(delta_event));
          }
        }
      }
    }
  }

  const auto* finish_reason = choice.FindString("finish_reason");
  if (finish_reason && !finish_reason->empty()) {
    ctx.pending_finish_reason = *finish_reason;
    if (!ctx.pending_usage.has_value()) {
      if (const base::DictValue* usage = chunk.FindDict("usage")) {
        ctx.pending_usage = BuildUsageDict(*usage);
      }
    }
    MaybeEmitFinish(ctx);
  }
}

}  // namespace

// --- StreamBodyConsumer ---

class OllamaProvider::StreamBodyConsumer
    : public network::SimpleURLLoaderStreamConsumer {
 public:
  using DataCallback = base::RepeatingCallback<void(std::string_view)>;
  using CompleteCallback = base::OnceCallback<void(bool)>;

  StreamBodyConsumer(DataCallback data_callback,
                     CompleteCallback complete_callback)
      : data_callback_(std::move(data_callback)),
        complete_callback_(std::move(complete_callback)) {}

  void OnDataReceived(std::string_view string_piece,
                      base::OnceClosure resume) override {
    data_callback_.Run(string_piece);
    std::move(resume).Run();
  }

  void OnComplete(bool success) override {
    std::move(complete_callback_).Run(success);
  }

  void OnRetry(base::OnceClosure start_retry) override {
    std::move(start_retry).Run();
  }

 private:
  DataCallback data_callback_;
  CompleteCallback complete_callback_;
};

// --- StreamContext ---

OllamaProvider::PendingRequest::PendingRequest() = default;
OllamaProvider::PendingRequest::PendingRequest(
    std::unique_ptr<network::SimpleURLLoader> loader,
    std::string id)
    : loader(std::move(loader)), id(std::move(id)) {}
OllamaProvider::PendingRequest::~PendingRequest() = default;
OllamaProvider::PendingRequest::PendingRequest(PendingRequest&&) = default;
OllamaProvider::PendingRequest& OllamaProvider::PendingRequest::operator=(
    PendingRequest&&) = default;

OllamaProvider::StreamContext::StreamContext() = default;
OllamaProvider::StreamContext::~StreamContext() = default;

// --- OllamaProvider ---

OllamaProvider::OllamaProvider(
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory)
    : url_loader_factory_(std::move(url_loader_factory)) {
  DETACH_FROM_SEQUENCE(sequence_checker_);
}

OllamaProvider::~OllamaProvider() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

std::string OllamaProvider::GetId() const {
  return "ollama";
}

std::string OllamaProvider::GetDisplayName() const {
  return "Ollama (Local)";
}

LlmProviderType OllamaProvider::GetProviderType() const {
  return LlmProviderType::kLocal;
}

std::vector<ModelInfo> OllamaProvider::GetSupportedModels() const {
  return fetched_models_;
}

std::string OllamaProvider::GetDefaultModelId() const {
  // Default-model selection is owned by the TS layer (nexus_web).
  return std::string();
}

bool OllamaProvider::SupportsStreaming() const {
  return true;
}

bool OllamaProvider::SupportsTools() const {
  return true;
}

bool OllamaProvider::IsConfigured() const {
  // Keyless: usable once it has somewhere to send requests — the guest's
  // catalog default or a user override.
  return !base_url_.empty();
}

bool OllamaProvider::Configure(const base::DictValue& config) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  ConfigureUserAgent(config);

  // A non-empty base_url is a user override; an empty one means "use the
  // default" and drops a previous override. Absent leaves it unchanged.
  if (const auto* base_url = config.FindString("base_url")) {
    base_url_overridden_ = !base_url->empty();
    base_url_ = base_url_overridden_ ? *base_url : default_base_url_;
  }

  const auto* model = config.FindString("model");
  if (model && !model->empty()) {
    configured_model_ = *model;
  }

  return true;
}

std::string OllamaProvider::GetDefaultBaseUrl() const {
  return default_base_url_;
}

void OllamaProvider::SetDefaultBaseUrl(const std::string& url) {
  default_base_url_ = url;
  if (!base_url_overridden_) {
    base_url_ = default_base_url_;
  }
}

std::string OllamaProvider::GetBaseUrl() const {
  return base_url_;
}

void OllamaProvider::DoGenerate(const base::DictValue& call_options,
                                GenerateCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  base::DictValue body = BuildRequestBody(call_options, /*stream=*/false);

  auto resource_request = std::make_unique<network::ResourceRequest>();
  resource_request->url =
      GURL(base::StrCat({base_url_, "/v1/chat/completions"}));
  resource_request->method = "POST";
  resource_request->headers.SetHeader("Content-Type", "application/json");
  ApplyUserAgent(resource_request->headers);

  auto loader = network::SimpleURLLoader::Create(std::move(resource_request),
                                                 kOllamaTrafficAnnotation);

  std::string body_json;
  base::JSONWriter::Write(body, &body_json);
  loader->AttachStringForUpload(body_json, "application/json");

  std::string request_id = ResolveRequestId(call_options);
  auto* loader_ptr = loader.get();

  pending_requests_[request_id] =
      PendingRequest{std::move(loader), request_id};

  loader_ptr->DownloadToString(
      url_loader_factory_.get(),
      base::BindOnce(&OllamaProvider::OnGenerateComplete,
                     weak_factory_.GetWeakPtr(), request_id,
                     std::move(callback)),
      kMaxResponseSizeBytes);
}

void OllamaProvider::DoStream(const base::DictValue& call_options,
                              StreamChunkCallback chunk_callback,
                              StreamDoneCallback done_callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // Wire chunk/done through BindPostTask: the host/guest chain behind them
  // can call back into CancelRequest synchronously, which erases
  // active_streams_ and destroys the StreamContext (including the consumer
  // whose bound request-id string is still referenced on the stack) — a
  // use-after-free. Posting breaks the re-entrancy; same-sequence posting
  // preserves chunk→done ordering.
  auto task_runner = base::SequencedTaskRunner::GetCurrentDefault();
  chunk_callback = base::BindPostTask(task_runner, std::move(chunk_callback));
  done_callback = base::BindPostTask(task_runner, std::move(done_callback));

  base::DictValue body = BuildRequestBody(call_options, /*stream=*/true);

  auto resource_request = std::make_unique<network::ResourceRequest>();
  resource_request->url =
      GURL(base::StrCat({base_url_, "/v1/chat/completions"}));
  resource_request->method = "POST";
  resource_request->headers.SetHeader("Content-Type", "application/json");
  ApplyUserAgent(resource_request->headers);

  auto loader = network::SimpleURLLoader::Create(std::move(resource_request),
                                                 kOllamaTrafficAnnotation);

  std::string body_json;
  base::JSONWriter::Write(body, &body_json);
  loader->AttachStringForUpload(body_json, "application/json");

  std::string request_id = ResolveRequestId(call_options);

  auto consumer = std::make_unique<StreamBodyConsumer>(
      base::BindRepeating(&OllamaProvider::OnStreamDataReceived,
                          weak_factory_.GetWeakPtr(), request_id),
      base::BindOnce(&OllamaProvider::OnStreamComplete,
                     weak_factory_.GetWeakPtr(), request_id));

  auto ctx = std::make_unique<StreamContext>();
  ctx->id = request_id;
  ctx->chunk_callback = std::move(chunk_callback);
  ctx->done_callback = std::move(done_callback);
  ctx->loader = std::move(loader);
  ctx->consumer = std::move(consumer);

  auto* loader_raw = ctx->loader.get();
  auto* consumer_raw = ctx->consumer.get();
  active_streams_[request_id] = std::move(ctx);

  loader_raw->DownloadAsStream(url_loader_factory_.get(), consumer_raw);
}

void OllamaProvider::CancelRequest(const std::string& request_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  pending_requests_.erase(request_id);
  active_streams_.erase(request_id);
}

void OllamaProvider::FetchAvailableModels(FetchModelsCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // Coalesce concurrent fetches — replacing models_loader_ mid-flight would
  // silently drop the earlier caller's callback (SimpleURLLoader never runs
  // it on destruction), hanging NexusLlmHost::Catalog()'s barrier.
  if (models_loader_) {
    pending_models_callbacks_.push_back(std::move(callback));
    return;
  }
  callback = base::BindOnce(
      [](base::WeakPtr<OllamaProvider> self, FetchModelsCallback primary,
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
      weak_factory_.GetWeakPtr(), std::move(callback));

  auto resource_request = std::make_unique<network::ResourceRequest>();
  resource_request->url = GURL(base::StrCat({base_url_, "/api/tags"}));
  resource_request->method = "GET";
  ApplyUserAgent(resource_request->headers);

  models_loader_ = network::SimpleURLLoader::Create(
      std::move(resource_request), kOllamaTrafficAnnotation);
  models_loader_->SetTimeoutDuration(
      base::Seconds(kModelFetchTimeoutSeconds));

  models_loader_->DownloadToString(
      url_loader_factory_.get(),
      base::BindOnce(&OllamaProvider::OnFetchModelsComplete,
                     weak_factory_.GetWeakPtr(), std::move(callback)),
      kMaxResponseSizeBytes);
}

// --- Request translation ---

base::DictValue OllamaProvider::BuildRequestBody(
    const base::DictValue& call_options,
    bool stream) const {
  base::DictValue body;
  body.Set("model", ResolveModel(call_options));
  body.Set("stream", stream);
  if (stream) {
    base::DictValue stream_options;
    stream_options.Set("include_usage", true);
    body.Set("stream_options", std::move(stream_options));
  }

  const auto* prompt = call_options.FindList("prompt");
  if (prompt) {
    body.Set("messages", TranslateMessages(*prompt));
  }

  auto max_tokens = call_options.FindInt("maxTokens");
  if (max_tokens) {
    body.Set("max_tokens", *max_tokens);
  }

  auto temperature = call_options.FindDouble("temperature");
  if (temperature) {
    body.Set("temperature", *temperature);
  }

  auto top_p = call_options.FindDouble("topP");
  if (top_p) {
    body.Set("top_p", *top_p);
  }

  const auto* stop = call_options.FindList("stopSequences");
  if (stop) {
    body.Set("stop", stop->Clone());
  }

  auto seed = call_options.FindInt("seed");
  if (seed) {
    body.Set("seed", *seed);
  }

  const auto* mode = call_options.FindDict("mode");
  if (mode) {
    const auto* type = mode->FindString("type");
    if (type && *type == "object-json") {
      base::DictValue response_format;
      response_format.Set("type", "json_object");
      body.Set("response_format", std::move(response_format));
    }

    const auto* tools = mode->FindList("tools");
    if (tools) {
      base::ListValue openai_tools;
      for (const auto& tool : *tools) {
        if (tool.is_dict()) {
          openai_tools.Append(TranslateToolDefinition(tool.GetDict()));
        }
      }
      body.Set("tools", std::move(openai_tools));
    }

    const auto* tool_choice_str = mode->FindString("toolChoice");
    if (tool_choice_str) {
      body.Set("tool_choice", *tool_choice_str);
    } else {
      const auto* tool_choice_dict = mode->FindDict("toolChoice");
      if (tool_choice_dict) {
        body.Set("tool_choice", tool_choice_dict->Clone());
      }
    }
  }

  // Merge TS-built provider-specific fields last so they override base params.
  if (const auto* ext = call_options.FindDict("providerBodyExtension")) {
    for (auto [key, val] : *ext) {
      body.Set(key, val.Clone());
    }
  }

  return body;
}

base::ListValue OllamaProvider::TranslateMessages(
    const base::ListValue& prompt) const {
  base::ListValue messages;

  for (const auto& msg_val : prompt) {
    if (!msg_val.is_dict()) {
      continue;
    }
    const auto& msg = msg_val.GetDict();
    const auto* role = msg.FindString("role");
    if (!role) {
      continue;
    }

    base::DictValue openai_msg;
    openai_msg.Set("role", *role);

    const auto* content_list = msg.FindList("content");
    if (!content_list) {
      const auto* content_str = msg.FindString("content");
      if (content_str) {
        openai_msg.Set("content", *content_str);
      }
      messages.Append(std::move(openai_msg));
      continue;
    }

    if (*role == "tool") {
      // Ollama speaks the OpenAI-compatible format: one `role:"tool"`
      // message per result part, images in a follow-up `user` message
      // after the whole batch (see openai_tool_result_messages.h).
      AppendChatCompletionToolMessages(*content_list, messages);
      continue;
    }

    if (*role == "assistant") {
      std::string text_content;
      base::ListValue tool_calls;

      for (const auto& part_val : *content_list) {
        if (!part_val.is_dict()) {
          continue;
        }
        const auto& part = part_val.GetDict();
        const auto* type = part.FindString("type");
        if (!type) {
          continue;
        }

        if (*type == "text") {
          const auto* text = part.FindString("text");
          if (text) {
            text_content += *text;
          }
        } else if (*type == "tool-call") {
          base::DictValue tc;
          const auto* id = part.FindString("toolCallId");
          if (id) {
            tc.Set("id", *id);
          }
          tc.Set("type", "function");

          base::DictValue function;
          const auto* name = part.FindString("toolName");
          if (name) {
            function.Set("name", *name);
          }
          const auto* args = part.Find("args");
          if (args) {
            std::string json;
            base::JSONWriter::Write(*args, &json);
            function.Set("arguments", json);
          }
          tc.Set("function", std::move(function));
          tool_calls.Append(std::move(tc));
        }
      }

      if (!text_content.empty()) {
        openai_msg.Set("content", text_content);
      }
      if (!tool_calls.empty()) {
        openai_msg.Set("tool_calls", std::move(tool_calls));
      }
      messages.Append(std::move(openai_msg));
      continue;
    }

    // User / system messages: check for images.
    bool has_image = false;
    for (const auto& part_val : *content_list) {
      if (part_val.is_dict()) {
        const auto* type = part_val.GetDict().FindString("type");
        if (type && *type == "image") {
          has_image = true;
          break;
        }
      }
    }

    if (has_image) {
      base::ListValue parts;
      for (const auto& part_val : *content_list) {
        if (!part_val.is_dict()) {
          continue;
        }
        const auto& part = part_val.GetDict();
        const auto* type = part.FindString("type");
        if (!type) {
          continue;
        }

        if (*type == "text") {
          base::DictValue text_part;
          text_part.Set("type", "text");
          const auto* text = part.FindString("text");
          if (text) {
            text_part.Set("text", *text);
          }
          parts.Append(std::move(text_part));
        } else if (*type == "image") {
          base::DictValue image_part;
          image_part.Set("type", "image_url");
          base::DictValue image_url;
          const auto* data = part.FindString("data");
          const auto* mime = part.FindString("mimeType");
          if (data && mime) {
            image_url.Set("url",
                          base::StrCat({"data:", *mime, ";base64,", *data}));
          }
          image_part.Set("image_url", std::move(image_url));
          parts.Append(std::move(image_part));
        } else if (*type == "document") {
          base::DictValue text_part;
          text_part.Set("type", "text");
          text_part.Set("text",
                        DocumentUnavailableMarker(part.FindString("mimeType")));
          parts.Append(std::move(text_part));
        }
      }
      openai_msg.Set("content", std::move(parts));
    } else {
      std::string text_content;
      for (const auto& part_val : *content_list) {
        if (part_val.is_dict()) {
          const auto& part = part_val.GetDict();
          const auto* type = part.FindString("type");
          if (type && *type == "document") {
            // No document part on this wire: say so rather than drop it.
            text_content += DocumentUnavailableMarker(part.FindString("mimeType"));
            continue;
          }
          const auto* text = part.FindString("text");
          if (text) {
            text_content += *text;
          }
        }
      }
      openai_msg.Set("content", text_content);
    }

    messages.Append(std::move(openai_msg));
  }

  return messages;
}

base::DictValue OllamaProvider::TranslateToolDefinition(
    const base::DictValue& tool) const {
  base::DictValue result;
  result.Set("type", "function");

  base::DictValue function;
  const auto* name = tool.FindString("name");
  if (name) {
    function.Set("name", *name);
  }
  const auto* description = tool.FindString("description");
  if (description) {
    function.Set("description", *description);
  }
  const auto* parameters = tool.FindDict("parameters");
  if (parameters) {
    function.Set("parameters", parameters->Clone());
  }
  result.Set("function", std::move(function));

  return result;
}

std::string OllamaProvider::ResolveModel(
    const base::DictValue& call_options) const {
  const auto* model = call_options.FindString("model");
  if (model && !model->empty()) {
    return *model;
  }
  // Falls back to whatever Configure() stored. If nothing is stored the
  // request will ship with an empty model id and fail loud at the API.
  return configured_model_;
}

// --- Response parsing ---

GenerateResult OllamaProvider::ParseResponse(
    const base::DictValue& response) const {
  GenerateResult result;

  const auto* error = response.FindDict("error");
  if (error) {
    const auto* message = error->FindString("message");
    result.finish_reason = FinishReason::kError;
    result.text = message ? *message : "Unknown API error";
    return result;
  }

  const auto* choices = response.FindList("choices");
  if (!choices || choices->empty()) {
    result.finish_reason = FinishReason::kError;
    result.text = "No choices in response";
    return result;
  }

  const auto& first_choice = (*choices)[0];
  if (!first_choice.is_dict()) {
    result.finish_reason = FinishReason::kError;
    return result;
  }
  const auto& choice = first_choice.GetDict();

  const auto* finish = choice.FindString("finish_reason");
  result.finish_reason =
      finish ? TranslateOpenAiFinishReason(*finish) : FinishReason::kUnknown;

  const auto* message = choice.FindDict("message");
  if (message) {
    const auto* content = message->FindString("content");
    if (content) {
      result.text = *content;
    }

    if (std::string thought = ExtractChatCompletionReasoningText(*message);
        !thought.empty()) {
      base::DictValue part;
      part.Set("type", "text");
      part.Set("text", std::move(thought));
      std::vector<base::DictValue> parts;
      parts.push_back(std::move(part));
      result.reasoning = std::move(parts);
    }

    const auto* tool_calls = message->FindList("tool_calls");
    if (tool_calls) {
      std::vector<base::DictValue> translated_calls;
      for (const auto& tc_val : *tool_calls) {
        if (!tc_val.is_dict()) {
          continue;
        }
        const auto& tc = tc_val.GetDict();
        base::DictValue translated;
        translated.Set("toolCallType", "function");

        const auto* id = tc.FindString("id");
        if (id) {
          translated.Set("toolCallId", *id);
        }

        const auto* function = tc.FindDict("function");
        if (function) {
          const auto* name = function->FindString("name");
          if (name) {
            translated.Set("toolName", *name);
          }
          const auto* args_str = function->FindString("arguments");
          if (args_str) {
            auto parsed_args =
                base::JSONReader::ReadDict(*args_str, base::JSON_PARSE_RFC);
            if (parsed_args) {
              translated.Set("args", std::move(*parsed_args));
            }
          }
        }
        translated_calls.push_back(std::move(translated));
      }
      result.tool_calls = std::move(translated_calls);
    }
  }

  const auto* usage = response.FindDict("usage");
  if (usage) {
    auto pt = usage->FindInt("prompt_tokens");
    if (pt) {
      result.prompt_tokens = *pt;
    }
    auto ct = usage->FindInt("completion_tokens");
    if (ct) {
      result.completion_tokens = *ct;
    }
  }

  return result;
}


// --- Network callbacks ---

void OllamaProvider::OnGenerateComplete(
    const std::string& request_id,
    GenerateCallback callback,
    std::optional<std::string> response_body) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  auto it = pending_requests_.find(request_id);
  if (it == pending_requests_.end()) {
    return;
  }

  auto pending = std::move(it->second);
  pending_requests_.erase(it);

  if (!response_body) {
    int net_error = pending.loader->NetError();
    DLOG(ERROR) << "Ollama request failed, net error: " << net_error;
    GenerateResult result;
    result.finish_reason = FinishReason::kError;
    std::move(callback).Run(
        std::move(result),
        base::StrCat({"Network error (", base::NumberToString(net_error),
                      "). Is Ollama running at ", base_url_, "?"}));
    return;
  }

  auto parsed =
      base::JSONReader::ReadDict(*response_body, base::JSON_PARSE_RFC);
  if (!parsed) {
    DLOG(ERROR) << "Failed to parse Ollama response as JSON";
    GenerateResult result;
    result.finish_reason = FinishReason::kError;
    // Recover a message from gateway/proxy error bodies (e.g. an
    // SSE-framed or wrapped error from a reverse proxy in front of
    // ollama) before falling back to the generic message.
    std::string error = ExtractApiErrorMessage(*response_body);
    if (error.empty()) {
      error = "Invalid JSON response";
    }
    std::move(callback).Run(std::move(result), std::move(error));
    return;
  }

  GenerateResult result = ParseResponse(*parsed);
  result.raw_response = parsed->Clone();
  std::move(callback).Run(std::move(result), std::nullopt);
}

void OllamaProvider::OnStreamDataReceived(std::string request_id,
                                          std::string_view string_piece) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // `request_id` is a by-value copy on purpose: the bound argument lives in
  // the StreamBodyConsumer owned by the StreamContext this method may cause
  // to be destroyed. See the OpenAI provider's field crash for the dangling
  // const-ref variant of this pattern.

  auto it = active_streams_.find(request_id);
  if (it == active_streams_.end()) {
    return;
  }

  auto& ctx = *it->second;
  for (const std::string& line : ctx.line_splitter.Feed(string_piece)) {
    if (!line.empty()) {
      ProcessNdjsonLine(ctx, line);
    }
  }
}

void OllamaProvider::OnStreamComplete(std::string request_id, bool success) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // `request_id` by value — the bound argument is destroyed with the
  // StreamContext erased below.

  auto it = active_streams_.find(request_id);
  if (it == active_streams_.end()) {
    return;
  }

  auto ctx = std::move(it->second);
  active_streams_.erase(it);

  if (!success) {
    DLOG(ERROR) << "Ollama stream failed";
    std::move(ctx->done_callback)
        .Run(base::StrCat(
            {"Stream error. Is Ollama running at ", base_url_, "?"}));
    return;
  }

  if (std::string tail = ctx->line_splitter.DrainRemaining(); !tail.empty()) {
    // Ollama's NDJSON stream may close without a trailing newline; treat
    // the final un-terminated line as a regular record.
    if (!tail.empty() && tail.back() == '\r') {
      tail.pop_back();
    }
    if (!tail.empty()) {
      ProcessNdjsonLine(*ctx, tail);
    }
  }

  if (!ctx->finish_emitted && ctx->pending_finish_reason.has_value()) {
    base::DictValue chunk;
    chunk.Set("type", "finish");
    chunk.Set("finishReason", *ctx->pending_finish_reason);
    if (ctx->pending_usage.has_value()) {
      chunk.Set("usage", std::move(*ctx->pending_usage));
      ctx->pending_usage.reset();
    }
    ctx->finish_emitted = true;
    ctx->chunk_callback.Run(std::move(chunk));
  }

  std::move(ctx->done_callback).Run(std::nullopt);
}

void OllamaProvider::ProcessNdjsonLine(StreamContext& ctx,
                                       const std::string& line) {
  // The request URL is Ollama's OpenAI-compat endpoint
  // (`/v1/chat/completions`), which emits OpenAI-style SSE — lines look like
  // `data: {json}` (with a leading `data:` prefix), and the stream ends with
  // `data: [DONE]`. There are also empty lines between frames and occasional
  // `:` / `event:` comment lines. Despite the `ProcessNdjsonLine` name (held
  // over from an earlier native-`/api/chat` implementation), this function
  // must tolerate SSE framing. Without this, every JSON parse fails because
  // the raw `data:` prefix isn't valid JSON, the entire stream is silently
  // dropped, and the assistant reply lands empty.
  std::string_view payload(line);

  // Skip SSE comment / event-name lines — they don't carry JSON.
  if (payload.empty() || payload.front() == ':')
    return;
  if (payload.substr(0, 6) == "event:")
    return;

  if (payload.substr(0, 5) == "data:") {
    payload.remove_prefix(5);
    // Tolerate `data:` both with and without the spec-recommended space.
    while (!payload.empty() && payload.front() == ' ')
      payload.remove_prefix(1);
    if (payload == "[DONE]")
      return;
  }

  auto parsed =
      base::JSONReader::ReadDict(std::string(payload), base::JSON_PARSE_RFC);
  if (!parsed) {
    return;
  }
  EmitStreamEvents(ctx, *parsed);
}

// --- Dynamic model listing ---

void OllamaProvider::OnFetchModelsComplete(
    FetchModelsCallback callback,
    std::optional<std::string> response_body) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  models_loader_.reset();

  // Capability flags and context-window sizing live in the TS layer. The
  // Ollama catalogue is whatever `/api/tags` returns on the user's local
  // server; if the call fails there is nothing to fall back to.
  if (!response_body) {
    DLOG(ERROR) << "Failed to fetch Ollama models from " << base_url_;
    std::move(callback).Run({});
    return;
  }

  // Cache the raw JSON as soon as we have a network-successful body. Any
  // downstream parse failure should still leave the prefs cache populated so
  // the TS layer has something to render.
  last_raw_catalog_json_ = *response_body;

  auto parsed =
      base::JSONReader::ReadDict(*response_body, base::JSON_PARSE_RFC);
  if (!parsed) {
    DLOG(ERROR) << "Failed to parse Ollama /api/tags response";
    std::move(callback).Run({});
    return;
  }

  const auto* models_list = parsed->FindList("models");
  if (!models_list || models_list->empty()) {
    std::move(callback).Run({});
    return;
  }

  std::vector<ModelInfo> models;
  for (const auto& model_val : *models_list) {
    if (!model_val.is_dict()) {
      continue;
    }
    const auto& model = model_val.GetDict();
    const auto* name = model.FindString("name");
    if (!name || name->empty()) {
      continue;
    }

    ModelInfo info;
    info.id = *name;
    auto colon_pos = name->find(':');
    info.name =
        (colon_pos != std::string::npos) ? name->substr(0, colon_pos) : *name;
    models.push_back(std::move(info));
  }

  fetched_models_ = models;
  std::move(callback).Run(std::move(models));
}

void OllamaProvider::LoadCachedCatalogJson(const std::string& raw_json) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (raw_json.empty())
    return;
  OnFetchModelsComplete(base::DoNothing(), raw_json);
}

std::string OllamaProvider::GetLastFetchedRawCatalogJson() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return last_raw_catalog_json_;
}

}  // namespace nexus
