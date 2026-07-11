#include "custom_browser/browser/nexus/llm/providers/openai/openai_provider.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/uuid.h"
#include "custom_browser/browser/nexus/llm/llm_line_splitter.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/cpp/simple_url_loader_stream_consumer.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "url/gurl.h"

namespace nexus {

namespace {

constexpr std::string_view kOpenAiProviderId = "openai";
constexpr std::string_view kOpenAiDisplayName = "OpenAI";
constexpr std::string_view kOpenAiDefaultBaseUrl =
    "https://api.openai.com/v1";

constexpr int kMaxResponseSizeBytes = 4 * 1024 * 1024;

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("nexus_openai_llm", R"(
      semantics {
        sender: "Nexus AI Agent Platform"
        description:
          "Sends chat completion requests to the OpenAI API on behalf of "
          "the user's configured AI agent. Requests contain conversation "
          "messages and optional tool definitions."
        trigger:
          "User interacts with the Nexus AI agent panel, triggering an "
          "LLM inference request through the configured OpenAI provider."
        data:
          "Conversation messages, model parameters, and optional tool "
          "schemas. May include user-authored text and page content the "
          "user has explicitly shared with the agent."
        destination: OTHER
        internal {
          contacts { email: "nicke@nicke.dev" }
        }
        user_data {
          type: OTHER
        }
        last_reviewed: "2026-03-13"
      }
      policy {
        cookies_allowed: NO
        setting:
          "Users configure their own OpenAI API key in Shift Browser "
          "settings under the Nexus AI section. No requests are sent "
          "without an API key."
        policy_exception_justification:
          "User-initiated AI feature with explicit opt-in and API key "
          "configuration."
      }
    )");

std::string ResolveRequestId(const base::DictValue& call_options) {
  if (const auto* request_id = call_options.FindString("requestId");
      request_id && !request_id->empty()) {
    return *request_id;
  }
  return base::Uuid::GenerateRandomV4().AsLowercaseString();
}

}  // namespace

// ---------------------------------------------------------------------------
// StreamConsumer — per-request SSE data pipe consumer
// ---------------------------------------------------------------------------

class OpenAiProvider::StreamConsumer
    : public network::SimpleURLLoaderStreamConsumer {
 public:
  StreamConsumer(base::WeakPtr<OpenAiProvider> provider,
                 std::string request_id)
      : provider_(std::move(provider)),
        request_id_(std::move(request_id)) {}

  void OnDataReceived(std::string_view data,
                      base::OnceClosure resume) override {
    if (provider_) {
      provider_->OnStreamDataReceived(request_id_, data);
    }
    std::move(resume).Run();
  }

  void OnComplete(bool success) override {
    if (provider_) {
      provider_->OnStreamComplete(request_id_, success);
    }
  }

  void OnRetry(base::OnceClosure start_retry) override {}

 private:
  base::WeakPtr<OpenAiProvider> provider_;
  std::string request_id_;
};

// ---------------------------------------------------------------------------
// StreamContext — per-stream state
// ---------------------------------------------------------------------------

struct OpenAiProvider::StreamContext {
  StreamContext() = default;

  ~StreamContext() {
    // Destroy loader first so it cannot invoke the consumer during teardown.
    loader.reset();
  }

  StreamContext(const StreamContext&) = delete;
  StreamContext& operator=(const StreamContext&) = delete;

  std::unique_ptr<StreamConsumer> consumer;
  std::unique_ptr<network::SimpleURLLoader> loader;

  StreamChunkCallback chunk_callback;
  StreamDoneCallback done_callback;

  LineSplitter line_splitter;
  std::string raw_response;
  std::string id;

  // Track in-flight tool calls by SSE index to correlate deltas.
  std::map<int, std::string> tool_call_ids;

  // With stream_options.include_usage=true OpenAI sends finish_reason and
  // usage in two separate SSE events. Buffer both so we can emit a single
  // "finish" chunk with usage attached.
  std::optional<std::string> pending_finish_reason;
  std::optional<base::DictValue> pending_usage;
  bool finish_emitted = false;
};

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

OpenAiProvider::OpenAiProvider(
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory)
    : OpenAiProvider(std::move(url_loader_factory),
                     std::string(kOpenAiProviderId),
                     std::string(kOpenAiDisplayName),
                     std::string(kOpenAiDefaultBaseUrl)) {}

OpenAiProvider::OpenAiProvider(
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
    std::string provider_id,
    std::string display_name,
    std::string default_base_url)
    : provider_id_(std::move(provider_id)),
      display_name_(std::move(display_name)),
      default_base_url_(std::move(default_base_url)),
      base_url_(default_base_url_),
      url_loader_factory_(std::move(url_loader_factory)) {}

OpenAiProvider::~OpenAiProvider() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

// ---------------------------------------------------------------------------
// LlmProvider property accessors
// ---------------------------------------------------------------------------

std::string OpenAiProvider::GetId() const {
  return provider_id_;
}

std::string OpenAiProvider::GetDisplayName() const {
  return display_name_;
}

LlmProviderType OpenAiProvider::GetProviderType() const {
  return LlmProviderType::kCloud;
}

std::vector<ModelInfo> OpenAiProvider::GetSupportedModels() const {
  return fetched_models_;
}

std::string OpenAiProvider::GetDefaultModelId() const {
  // Default-model selection is owned by the TS layer (nexus_web).
  return std::string();
}

bool OpenAiProvider::SupportsStreaming() const {
  return true;
}

bool OpenAiProvider::SupportsTools() const {
  return true;
}

bool OpenAiProvider::IsConfigured() const {
  return !api_key_.empty();
}

bool OpenAiProvider::Configure(const base::DictValue& config) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  const std::string* api_key = config.FindString("api_key");
  if (!api_key || api_key->empty()) {
    DLOG(ERROR) << display_name_ << " provider requires a non-empty api_key";
    return false;
  }
  api_key_ = *api_key;

  if (const std::string* base_url = config.FindString("base_url");
      base_url && !base_url->empty()) {
    base_url_ = *base_url;
  }

  if (const std::string* model = config.FindString("model");
      model && !model->empty()) {
    configured_model_ = *model;
  }

  return true;
}

std::string OpenAiProvider::GetDefaultBaseUrl() const {
  return default_base_url_;
}

std::string OpenAiProvider::GetBaseUrl() const {
  return base_url_;
}

// ---------------------------------------------------------------------------
// Request translation — Vercel wire format → OpenAI API
// ---------------------------------------------------------------------------

std::string OpenAiProvider::ResolveModel(
    const base::DictValue& call_options) const {
  if (const std::string* model_id = call_options.FindString("modelId");
      model_id && !model_id->empty()) {
    return *model_id;
  }
  // Falls back to whatever Configure() stored. If nothing is stored the
  // request will ship with an empty model id and fail loud at the API —
  // that is intentional: the TS layer is authoritative for model selection.
  return configured_model_;
}

base::DictValue OpenAiProvider::BuildRequestBody(
    const base::DictValue& call_options,
    bool stream) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  base::DictValue body;
  body.Set("model", ResolveModel(call_options));

  if (const base::ListValue* prompt = call_options.FindList("prompt")) {
    body.Set("messages", TranslateMessages(*prompt));
  }

  if (stream) {
    body.Set("stream", true);
    base::DictValue stream_options;
    stream_options.Set("include_usage", true);
    body.Set("stream_options", std::move(stream_options));
  }

  if (const base::DictValue* mode = call_options.FindDict("mode")) {
    const std::string* type = mode->FindString("type");

    if (type && *type == "regular") {
      if (const base::ListValue* tools = mode->FindList("tools")) {
        base::ListValue openai_tools;
        for (const auto& tool_val : *tools) {
          if (const base::DictValue* tool = tool_val.GetIfDict()) {
            openai_tools.Append(TranslateToolDefinition(*tool));
          }
        }
        if (!openai_tools.empty()) {
          body.Set("tools", std::move(openai_tools));
        }
      }

      if (const base::DictValue* tc_dict = mode->FindDict("toolChoice")) {
        const std::string* tc_type = tc_dict->FindString("type");
        if (tc_type) {
          if (*tc_type == "auto" || *tc_type == "none" ||
              *tc_type == "required") {
            body.Set("tool_choice", *tc_type);
          } else if (*tc_type == "tool") {
            if (const std::string* tool_name =
                    tc_dict->FindString("toolName")) {
              base::DictValue tc;
              tc.Set("type", "function");
              base::DictValue fn;
              fn.Set("name", *tool_name);
              tc.Set("function", std::move(fn));
              body.Set("tool_choice", std::move(tc));
            }
          }
        }
      }
    } else if (type && *type == "object-json") {
      if (const base::DictValue* schema = mode->FindDict("schema")) {
        base::DictValue rf;
        rf.Set("type", "json_schema");
        base::DictValue js;
        js.Set("name", "response");
        js.Set("strict", true);
        js.Set("schema", schema->Clone());
        rf.Set("json_schema", std::move(js));
        body.Set("response_format", std::move(rf));
      } else {
        base::DictValue rf;
        rf.Set("type", "json_object");
        body.Set("response_format", std::move(rf));
      }
    }
  }

  if (auto v = call_options.FindInt("maxTokens"))
    body.Set("max_tokens", *v);
  if (auto v = call_options.FindDouble("temperature"))
    body.Set("temperature", *v);
  if (auto v = call_options.FindDouble("topP"))
    body.Set("top_p", *v);
  if (const auto* v = call_options.FindList("stopSequences"))
    body.Set("stop", v->Clone());
  if (auto v = call_options.FindDouble("presencePenalty"))
    body.Set("presence_penalty", *v);
  if (auto v = call_options.FindDouble("frequencyPenalty"))
    body.Set("frequency_penalty", *v);
  if (auto v = call_options.FindInt("seed"))
    body.Set("seed", *v);

  // Merge TS-built provider-specific fields last so they override base params.
  if (const auto* ext = call_options.FindDict("providerBodyExtension")) {
    for (auto [key, val] : *ext) {
      body.Set(key, val.Clone());
    }
  }

  return body;
}

base::ListValue OpenAiProvider::TranslateMessages(
    const base::ListValue& prompt) const {
  base::ListValue messages;

  for (const auto& msg_val : prompt) {
    const base::DictValue* msg = msg_val.GetIfDict();
    if (!msg)
      continue;

    const std::string* role = msg->FindString("role");
    if (!role)
      continue;

    const base::ListValue* content_parts = msg->FindList("content");
    if (!content_parts)
      continue;

    // --- Tool-result messages (role == "tool") ---
    if (*role == "tool") {
      for (const auto& part_val : *content_parts) {
        const base::DictValue* part = part_val.GetIfDict();
        if (!part)
          continue;
        const std::string* pt = part->FindString("type");
        if (!pt || *pt != "tool-result")
          continue;

        base::DictValue tool_msg;
        tool_msg.Set("role", "tool");

        if (const std::string* tc_id = part->FindString("toolCallId"))
          tool_msg.Set("tool_call_id", *tc_id);

        std::string content;
        if (const base::ListValue* result_parts = part->FindList("result")) {
          for (const auto& rp_val : *result_parts) {
            if (const base::DictValue* rp = rp_val.GetIfDict()) {
              const std::string* rp_type = rp->FindString("type");
              if (rp_type && *rp_type == "text") {
                if (const std::string* text = rp->FindString("text")) {
                  if (!content.empty())
                    content += '\n';
                  content += *text;
                }
              }
            }
          }
        } else if (const std::string* result_str =
                       part->FindString("result")) {
          content = *result_str;
        } else if (const base::Value* result_val = part->Find("result")) {
          base::JSONWriter::Write(*result_val, &content);
        }
        tool_msg.Set("content", std::move(content));
        messages.Append(std::move(tool_msg));
      }
      continue;
    }

    // --- Assistant messages (may contain tool-call parts) ---
    if (*role == "assistant") {
      std::string text_content;
      base::ListValue tool_calls;
      bool has_tool_calls = false;

      for (const auto& part_val : *content_parts) {
        const base::DictValue* part = part_val.GetIfDict();
        if (!part)
          continue;
        const std::string* pt = part->FindString("type");
        if (!pt)
          continue;

        if (*pt == "tool-call") {
          has_tool_calls = true;
          base::DictValue tc;
          tc.Set("type", "function");
          if (const std::string* tc_id = part->FindString("toolCallId"))
            tc.Set("id", *tc_id);

          base::DictValue fn;
          if (const std::string* name = part->FindString("toolName"))
            fn.Set("name", *name);
          if (const base::DictValue* args = part->FindDict("args")) {
            std::string args_json;
            base::JSONWriter::Write(base::Value(args->Clone()), &args_json);
            fn.Set("arguments", std::move(args_json));
          } else {
            fn.Set("arguments", "{}");
          }
          tc.Set("function", std::move(fn));
          tool_calls.Append(std::move(tc));
        } else if (*pt == "text") {
          if (const std::string* text = part->FindString("text")) {
            if (!text_content.empty())
              text_content += '\n';
            text_content += *text;
          }
        }
      }

      base::DictValue assistant_msg;
      assistant_msg.Set("role", "assistant");
      if (!text_content.empty()) {
        assistant_msg.Set("content", std::move(text_content));
      } else if (!has_tool_calls) {
        assistant_msg.Set("content", "");
      }
      if (has_tool_calls)
        assistant_msg.Set("tool_calls", std::move(tool_calls));
      messages.Append(std::move(assistant_msg));
      continue;
    }

    // --- System / user messages ---
    bool has_images = false;
    for (const auto& part_val : *content_parts) {
      if (const base::DictValue* part = part_val.GetIfDict()) {
        const std::string* pt = part->FindString("type");
        if (pt && *pt == "image") {
          has_images = true;
          break;
        }
      }
    }

    if (has_images) {
      base::ListValue openai_content;
      for (const auto& part_val : *content_parts) {
        const base::DictValue* part = part_val.GetIfDict();
        if (!part)
          continue;
        const std::string* pt = part->FindString("type");
        if (!pt)
          continue;

        if (*pt == "text") {
          if (const std::string* text = part->FindString("text")) {
            base::DictValue text_part;
            text_part.Set("type", "text");
            text_part.Set("text", *text);
            openai_content.Append(std::move(text_part));
          }
        } else if (*pt == "image") {
          base::DictValue image_part;
          image_part.Set("type", "image_url");
          base::DictValue image_url;

          if (const std::string* url = part->FindString("image")) {
            image_url.Set("url", *url);
          } else if (const std::string* b64 = part->FindString("data")) {
            const std::string* mime = part->FindString("mimeType");
            std::string data_uri = "data:";
            data_uri += mime ? *mime : "image/png";
            data_uri += ";base64,";
            data_uri += *b64;
            image_url.Set("url", std::move(data_uri));
          }

          image_part.Set("image_url", std::move(image_url));
          openai_content.Append(std::move(image_part));
        }
      }

      base::DictValue openai_msg;
      openai_msg.Set("role", *role);
      openai_msg.Set("content", std::move(openai_content));
      messages.Append(std::move(openai_msg));
    } else {
      std::string combined;
      for (const auto& part_val : *content_parts) {
        if (const base::DictValue* part = part_val.GetIfDict()) {
          const std::string* pt = part->FindString("type");
          if (pt && *pt == "text") {
            if (const std::string* text = part->FindString("text")) {
              if (!combined.empty())
                combined += '\n';
              combined += *text;
            }
          }
        }
      }
      base::DictValue openai_msg;
      openai_msg.Set("role", *role);
      openai_msg.Set("content", std::move(combined));
      messages.Append(std::move(openai_msg));
    }
  }

  return messages;
}

base::DictValue OpenAiProvider::TranslateToolDefinition(
    const base::DictValue& tool) const {
  base::DictValue openai_tool;
  openai_tool.Set("type", "function");

  base::DictValue function;
  if (const std::string* name = tool.FindString("name"))
    function.Set("name", *name);
  if (const std::string* desc = tool.FindString("description"))
    function.Set("description", *desc);
  if (const base::DictValue* params = tool.FindDict("parameters"))
    function.Set("parameters", params->Clone());

  // Forward `strict: true` when the JS layer has opted into OpenAI
  // Structured Outputs (grammar-constrained sampling on tool inputs).
  // OpenAI places `strict` on the inner `function` object alongside
  // `name`/`description`/`parameters`. The JS layer is also
  // responsible for tightening the schema with
  // `additionalProperties: false` before this lands here; the OpenAI
  // API rejects strict tools that lack it.
  if (auto strict = tool.FindBool("strict"))
    function.Set("strict", *strict);

  openai_tool.Set("function", std::move(function));
  return openai_tool;
}

// ---------------------------------------------------------------------------
// Response parsing — OpenAI API → Vercel wire format
// ---------------------------------------------------------------------------

GenerateResult OpenAiProvider::ParseResponse(
    const base::DictValue& response) const {
  GenerateResult result;
  result.raw_response = response.Clone();

  const base::ListValue* choices = response.FindList("choices");
  if (!choices || choices->empty()) {
    result.finish_reason = FinishReason::kError;
    return result;
  }

  const base::DictValue* first_choice = (*choices)[0].GetIfDict();
  if (!first_choice) {
    result.finish_reason = FinishReason::kError;
    return result;
  }

  if (const std::string* fr = first_choice->FindString("finish_reason"))
    result.finish_reason = TranslateOpenAiFinishReason(*fr);

  const base::DictValue* message = first_choice->FindDict("message");
  if (!message) {
    result.finish_reason = FinishReason::kError;
    return result;
  }

  if (const std::string* content = message->FindString("content"))
    result.text = *content;

  if (const base::ListValue* tool_calls =
          message->FindList("tool_calls")) {
    std::vector<base::DictValue> translated;
    for (const auto& tc_val : *tool_calls) {
      const base::DictValue* tc = tc_val.GetIfDict();
      if (!tc)
        continue;

      base::DictValue vercel_tc;
      vercel_tc.Set("toolCallType", "function");
      if (const std::string* id = tc->FindString("id"))
        vercel_tc.Set("toolCallId", *id);

      if (const base::DictValue* fn = tc->FindDict("function")) {
        if (const std::string* name = fn->FindString("name"))
          vercel_tc.Set("toolName", *name);
        if (const std::string* args_str = fn->FindString("arguments")) {
          auto parsed =
              base::JSONReader::ReadDict(*args_str, base::JSON_PARSE_RFC);
          if (parsed)
            vercel_tc.Set("args", std::move(*parsed));
          else
            vercel_tc.Set("args", base::DictValue());
        }
      }

      translated.push_back(std::move(vercel_tc));
    }
    if (!translated.empty()) {
      result.tool_calls = std::move(translated);
      if (result.finish_reason == FinishReason::kUnknown)
        result.finish_reason = FinishReason::kToolCalls;
    }
  }

  if (const base::DictValue* usage = response.FindDict("usage")) {
    result.prompt_tokens = usage->FindInt("prompt_tokens").value_or(0);
    result.completion_tokens =
        usage->FindInt("completion_tokens").value_or(0);
  }

  return result;
}

// ---------------------------------------------------------------------------
// DoGenerate — non-streaming completion
// ---------------------------------------------------------------------------

void OpenAiProvider::DoGenerate(const base::DictValue& call_options,
                                GenerateCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (!IsConfigured()) {
    std::move(callback).Run(GenerateResult(),
                            display_name_ + " provider not configured");
    return;
  }

  std::string request_id = ResolveRequestId(call_options);
  base::DictValue body = BuildRequestBody(call_options, /*stream=*/false);

  std::string body_json;
  base::JSONWriter::Write(base::Value(std::move(body)), &body_json);

  auto resource_request = std::make_unique<network::ResourceRequest>();
  resource_request->url = GURL(base_url_ + "/chat/completions");
  resource_request->method = "POST";
  resource_request->headers.SetHeader("Authorization",
                                      "Bearer " + api_key_);
  resource_request->headers.SetHeader("Content-Type", "application/json");

  auto loader = network::SimpleURLLoader::Create(std::move(resource_request),
                                                 kTrafficAnnotation);
  loader->AttachStringForUpload(body_json, "application/json");
  loader->SetAllowHttpErrorResults(true);

  auto* loader_ptr = loader.get();
  pending_loaders_.emplace(request_id, std::move(loader));

  loader_ptr->DownloadToString(
      url_loader_factory_.get(),
      base::BindOnce(&OpenAiProvider::OnGenerateComplete,
                     weak_factory_.GetWeakPtr(), request_id,
                     std::move(callback)),
      kMaxResponseSizeBytes);
}

void OpenAiProvider::OnGenerateComplete(
    const std::string& request_id,
    GenerateCallback callback,
    std::optional<std::string> response_body) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  int net_error = 0;
  int http_status = -1;

  auto it = pending_loaders_.find(request_id);
  if (it != pending_loaders_.end()) {
    net_error = it->second->NetError();
    if (const auto* info = it->second->ResponseInfo();
        info && info->headers) {
      http_status = info->headers->response_code();
    }
    pending_loaders_.erase(it);
  }

  if (!response_body) {
    std::string error = "Network error (net::ERR = " +
                        base::NumberToString(net_error) + ")";
    DLOG(ERROR) << display_name_ << " request failed: " << error;
    std::move(callback).Run(GenerateResult(), std::move(error));
    return;
  }

  auto parsed =
      base::JSONReader::ReadDict(*response_body, base::JSON_PARSE_RFC);
  if (!parsed) {
    DLOG(ERROR) << display_name_ << " returned non-JSON response";
    std::move(callback).Run(GenerateResult(),
                            "Invalid JSON in " + display_name_ + " response");
    return;
  }

  const base::DictValue& dict = *parsed;

  if (const base::DictValue* error_obj = dict.FindDict("error")) {
    std::string error_msg = display_name_ + " API error";
    if (const std::string* msg = error_obj->FindString("message"))
      error_msg = *msg;
    DLOG(ERROR) << display_name_ << " API error (HTTP " << http_status
                << "): " << error_msg;
    std::move(callback).Run(GenerateResult(), std::move(error_msg));
    return;
  }

  GenerateResult result = ParseResponse(dict);
  std::move(callback).Run(std::move(result), std::nullopt);
}

// ---------------------------------------------------------------------------
// DoStream — SSE streaming completion
// ---------------------------------------------------------------------------

void OpenAiProvider::DoStream(const base::DictValue& call_options,
                              StreamChunkCallback chunk_callback,
                              StreamDoneCallback done_callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (!IsConfigured()) {
    std::move(done_callback).Run(display_name_ + " provider not configured");
    return;
  }

  std::string request_id = ResolveRequestId(call_options);
  base::DictValue body = BuildRequestBody(call_options, /*stream=*/true);

  std::string body_json;
  base::JSONWriter::Write(base::Value(std::move(body)), &body_json);

  auto resource_request = std::make_unique<network::ResourceRequest>();
  resource_request->url = GURL(base_url_ + "/chat/completions");
  resource_request->method = "POST";
  resource_request->headers.SetHeader("Authorization",
                                      "Bearer " + api_key_);
  resource_request->headers.SetHeader("Content-Type", "application/json");

  auto loader = network::SimpleURLLoader::Create(std::move(resource_request),
                                                 kTrafficAnnotation);
  loader->AttachStringForUpload(body_json, "application/json");
  loader->SetAllowHttpErrorResults(true);

  auto consumer = std::make_unique<StreamConsumer>(
      weak_factory_.GetWeakPtr(), request_id);
  auto* consumer_ptr = consumer.get();
  auto* loader_ptr = loader.get();

  auto ctx = std::make_unique<StreamContext>();
  ctx->consumer = std::move(consumer);
  ctx->loader = std::move(loader);
  ctx->chunk_callback = std::move(chunk_callback);
  ctx->done_callback = std::move(done_callback);
  ctx->id = request_id;

  active_streams_.emplace(request_id, std::move(ctx));

  loader_ptr->DownloadAsStream(url_loader_factory_.get(), consumer_ptr);
}

void OpenAiProvider::OnStreamDataReceived(const std::string& request_id,
                                          std::string_view data) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  auto it = active_streams_.find(request_id);
  if (it == active_streams_.end())
    return;

  StreamContext& ctx = *it->second;
  ctx.raw_response.append(data);

  for (const std::string& line : ctx.line_splitter.Feed(data)) {
    ProcessSseLine(ctx, line);
  }
}

void OpenAiProvider::ProcessSseLine(StreamContext& ctx,
                                    std::string_view line) {
  if (line.empty() || line[0] == ':')
    return;

  if (!line.starts_with("data:"))
    return;

  std::string_view payload = line.substr(5);
  if (!payload.empty() && payload[0] == ' ')
    payload = payload.substr(1);

  if (payload == "[DONE]")
    return;

  auto parsed = base::JSONReader::ReadDict(payload, base::JSON_PARSE_RFC);
  if (!parsed)
    return;

  const base::DictValue& dict = *parsed;

  auto build_usage_dict = [](const base::DictValue& usage) {
    base::DictValue u;
    u.Set("promptTokens", usage.FindInt("prompt_tokens").value_or(0));
    u.Set("completionTokens",
          usage.FindInt("completion_tokens").value_or(0));
    if (const base::DictValue* details =
            usage.FindDict("prompt_tokens_details")) {
      if (auto cached = details->FindInt("cached_tokens")) {
        u.Set("cacheReadInputTokens", *cached);
      }
    }
    return u;
  };

  auto maybe_emit_finish = [&ctx]() {
    // OpenAI sends finish_reason and usage in separate SSE chunks when
    // stream_options.include_usage=true. Wait for both so the finish
    // chunk carries real usage numbers (OnStreamComplete flushes finish
    // without usage as a fallback if usage never arrives).
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
  };

  // Usage-only chunk (sent when stream_options.include_usage is true).
  const base::ListValue* choices = dict.FindList("choices");
  if (!choices || choices->empty()) {
    if (const base::DictValue* usage = dict.FindDict("usage")) {
      ctx.pending_usage = build_usage_dict(*usage);
      maybe_emit_finish();
    }
    return;
  }

  const base::DictValue* first = (*choices)[0].GetIfDict();
  if (!first)
    return;

  if (const base::DictValue* delta = first->FindDict("delta")) {
    // Text content delta.
    if (const std::string* content = delta->FindString("content")) {
      if (!content->empty()) {
        base::DictValue chunk;
        chunk.Set("type", "text-delta");
        chunk.Set("textDelta", *content);
        ctx.chunk_callback.Run(std::move(chunk));
      }
    }

    // Tool-call deltas.
    if (const base::ListValue* tool_calls =
            delta->FindList("tool_calls")) {
      for (const auto& tc_val : *tool_calls) {
        const base::DictValue* tc = tc_val.GetIfDict();
        if (!tc)
          continue;

        int index = tc->FindInt("index").value_or(0);

        if (const std::string* id = tc->FindString("id")) {
          ctx.tool_call_ids[index] = *id;

          base::DictValue chunk;
          chunk.Set("type", "tool-call-streaming-start");
          chunk.Set("toolCallId", *id);
          chunk.Set("toolCallType", "function");

          if (const base::DictValue* fn = tc->FindDict("function")) {
            if (const std::string* name = fn->FindString("name"))
              chunk.Set("toolName", *name);
          }
          ctx.chunk_callback.Run(std::move(chunk));
        }

        if (const base::DictValue* fn = tc->FindDict("function")) {
          if (const std::string* args = fn->FindString("arguments")) {
            if (!args->empty()) {
              base::DictValue chunk;
              chunk.Set("type", "tool-call-delta");
              auto id_it = ctx.tool_call_ids.find(index);
              if (id_it != ctx.tool_call_ids.end())
                chunk.Set("toolCallId", id_it->second);
              chunk.Set("argsTextDelta", *args);
              ctx.chunk_callback.Run(std::move(chunk));
            }
          }
        }
      }
    }
  }

  // Finish reason (null for intermediate chunks, string for the final one).
  if (const std::string* fr = first->FindString("finish_reason")) {
    if (!fr->empty()) {
      ctx.pending_finish_reason = *fr;
      // Usage may already be attached to this same SSE chunk (providers
      // without split usage/finish semantics).
      if (!ctx.pending_usage.has_value()) {
        if (const base::DictValue* usage = dict.FindDict("usage")) {
          ctx.pending_usage = build_usage_dict(*usage);
        }
      }
      maybe_emit_finish();
    }
  }
}

void OpenAiProvider::OnStreamComplete(const std::string& request_id,
                                      bool success) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  auto it = active_streams_.find(request_id);
  if (it == active_streams_.end())
    return;

  auto ctx = std::move(it->second);
  active_streams_.erase(it);

  // If a finish_reason was buffered but the finish chunk was never emitted
  // (e.g. usage arrived out of order or never arrived), flush it now so
  // downstream consumers still observe a terminal finish chunk with
  // whatever usage we managed to buffer.
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

  if (!success) {
    int net_error = ctx->loader ? ctx->loader->NetError() : -1;
    std::string error = "Stream network error (net::ERR = " +
                        base::NumberToString(net_error) + ")";
    DLOG(ERROR) << display_name_ << " stream failed: " << error;
    std::move(ctx->done_callback).Run(std::move(error));
    return;
  }

  // With SetAllowHttpErrorResults(true) the loader reports success even for
  // HTTP 4xx/5xx.  Check the status code and surface the API error body.
  if (ctx->loader) {
    if (const auto* info = ctx->loader->ResponseInfo();
        info && info->headers) {
      int status = info->headers->response_code();
      if (status >= 400) {
        std::string error = "HTTP " + base::NumberToString(status);
        auto parsed =
            base::JSONReader::ReadDict(ctx->raw_response, base::JSON_PARSE_RFC);
        if (parsed) {
          if (const auto* err = parsed->FindDict("error")) {
            if (const std::string* msg = err->FindString("message"))
              error = *msg;
          }
        }
        DLOG(ERROR) << display_name_ << " stream error: " << error;
        std::move(ctx->done_callback).Run(std::move(error));
        return;
      }
    }
  }

  std::move(ctx->done_callback).Run(std::nullopt);
}

// ---------------------------------------------------------------------------
// CancelRequest
// ---------------------------------------------------------------------------

void OpenAiProvider::CancelRequest(const std::string& request_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  pending_loaders_.erase(request_id);

  auto it = active_streams_.find(request_id);
  if (it != active_streams_.end()) {
    auto ctx = std::move(it->second);
    active_streams_.erase(it);
    if (ctx->done_callback)
      std::move(ctx->done_callback).Run("Cancelled");
  }
}

// ---------------------------------------------------------------------------
// FetchAvailableModels — GET /v1/models
// ---------------------------------------------------------------------------

void OpenAiProvider::FetchAvailableModels(FetchModelsCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (!IsConfigured()) {
    std::move(callback).Run({});
    return;
  }

  std::string models_url = base_url_ + "/models";

  auto request = std::make_unique<network::ResourceRequest>();
  request->url = GURL(models_url);
  request->method = "GET";
  request->headers.SetHeader("Authorization", "Bearer " + api_key_);

  models_loader_ =
      network::SimpleURLLoader::Create(std::move(request), kTrafficAnnotation);

  models_loader_->DownloadToString(
      url_loader_factory_.get(),
      base::BindOnce(&OpenAiProvider::OnFetchModelsComplete,
                     weak_factory_.GetWeakPtr(), std::move(callback)),
      kMaxResponseSizeBytes);
}

void OpenAiProvider::OnFetchModelsComplete(
    FetchModelsCallback callback,
    std::optional<std::string> response_body) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  models_loader_.reset();

  // Capability flags, context-window size and filtering/display overrides
  // all live in the TS layer. The native provider passes through whatever
  // the /v1/models endpoint returned. OpenAI's /v1/models lists every kind
  // of model (embeddings, audio, images, chat); TS is responsible for
  // filtering to chat-capable entries via the curated model config.
  if (!response_body) {
    DLOG(WARNING) << display_name_ << " models fetch failed";
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
    DLOG(WARNING) << display_name_ << " models: invalid JSON";
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
    info.name = *id;

    models.push_back(std::move(info));
  }

  fetched_models_ = models;
  std::move(callback).Run(std::move(models));
}

void OpenAiProvider::LoadCachedCatalogJson(const std::string& raw_json) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (raw_json.empty())
    return;
  OnFetchModelsComplete(base::DoNothing(), raw_json);
}

std::string OpenAiProvider::GetLastFetchedRawCatalogJson() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return last_raw_catalog_json_;
}

}  // namespace nexus
