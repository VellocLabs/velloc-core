#include "custom_browser/browser/nexus/api/llm/providers/google/google_provider.h"

#include "custom_browser/browser/nexus/api/llm/llm_error_extract.h"
#include "custom_browser/browser/nexus/nexus_secret_crypt.h"

#include <optional>
#include <utility>

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
#include "base/uuid.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader_stream_consumer.h"

namespace nexus {

namespace {

constexpr size_t kMaxResponseSizeBytes =
    network::SimpleURLLoader::kMaxBoundedStringDownloadSize;

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("nexus_google_llm_provider", R"(
      semantics {
        sender: "Nexus AI Agent Platform"
        description:
          "Sends requests to the Google AI (Gemini) API to generate text "
          "completions and function calls for the AI agent platform."
        trigger:
          "User interacts with an AI agent configured to use the Google AI "
          "provider."
        data:
          "Conversation messages, tool definitions, and generation "
          "parameters. Content depends on the user's conversation."
        destination: GOOGLE_OWNED_SERVICE
      }
      policy {
        cookies_allowed: NO
        setting:
          "Users configure the Google AI provider with their own API key "
          "through the Nexus agent platform settings."
      })");

FinishReason MapGoogleFinishReason(const std::string& reason) {
  if (reason == "STOP")
    return FinishReason::kStop;
  if (reason == "MAX_TOKENS")
    return FinishReason::kLength;
  if (reason == "SAFETY" || reason == "RECITATION")
    return FinishReason::kContentFilter;
  if (reason == "OTHER")
    return FinishReason::kOther;
  return FinishReason::kUnknown;
}

std::string ResolveRequestId(const base::DictValue& call_options) {
  if (const auto* request_id = call_options.FindString("requestId");
      request_id && !request_id->empty()) {
    return *request_id;
  }
  return base::Uuid::GenerateRandomV4().AsLowercaseString();
}

base::ListValue TranslateUserContentParts(const base::ListValue& parts) {
  base::ListValue google_parts;
  for (const auto& part_value : parts) {
    if (!part_value.is_dict())
      continue;
    const auto& part = part_value.GetDict();
    const std::string* type = part.FindString("type");
    if (!type)
      continue;

    if (*type == "text") {
      const std::string* text = part.FindString("text");
      if (text) {
        base::DictValue p;
        p.Set("text", *text);
        google_parts.Append(std::move(p));
      }
    } else if (*type == "image") {
      const std::string* mime_type = part.FindString("mimeType");
      const std::string* data = part.FindString("data");
      if (mime_type && data) {
        base::DictValue inline_data;
        inline_data.Set("mimeType", *mime_type);
        inline_data.Set("data", *data);
        base::DictValue p;
        p.Set("inlineData", std::move(inline_data));
        google_parts.Append(std::move(p));
      }
    } else if (*type == "document") {
      // Gemini reads a PDF as inlineData, as the tool-result path sends it.
      const std::string* data = part.FindString("data");
      if (data) {
        const std::string* mime_type = part.FindString("mimeType");
        base::DictValue inline_data;
        inline_data.Set("mimeType",
                        mime_type ? *mime_type : std::string("application/pdf"));
        inline_data.Set("data", *data);
        base::DictValue p;
        p.Set("inlineData", std::move(inline_data));
        google_parts.Append(std::move(p));
      }
    }
  }
  return google_parts;
}

base::ListValue TranslateAssistantContentParts(
    const base::ListValue& parts) {
  base::ListValue google_parts;
  for (const auto& part_value : parts) {
    if (!part_value.is_dict())
      continue;
    const auto& part = part_value.GetDict();
    const std::string* type = part.FindString("type");
    if (!type)
      continue;

    if (*type == "text") {
      const std::string* text = part.FindString("text");
      if (text && !text->empty()) {
        base::DictValue p;
        p.Set("text", *text);
        google_parts.Append(std::move(p));
      }
    } else if (*type == "tool-call") {
      const std::string* tool_name = part.FindString("toolName");
      if (!tool_name)
        continue;

      base::DictValue func_call;
      func_call.Set("name", *tool_name);

      const std::string* args_str = part.FindString("args");
      if (args_str) {
        auto args_value =
            base::JSONReader::ReadDict(*args_str, base::JSON_PARSE_RFC);
        if (args_value) {
          func_call.Set("args", std::move(*args_value));
        }
      }
      const auto* args_dict = part.FindDict("args");
      if (args_dict) {
        func_call.Set("args", args_dict->Clone());
      }

      base::DictValue p;
      p.Set("functionCall", std::move(func_call));
      google_parts.Append(std::move(p));
    }
  }
  return google_parts;
}

base::ListValue TranslateToolResultParts(const base::ListValue& parts) {
  base::ListValue google_parts;
  for (const auto& part_value : parts) {
    if (!part_value.is_dict())
      continue;
    const auto& part = part_value.GetDict();
    const std::string* type = part.FindString("type");
    if (!type || *type != "tool-result")
      continue;

    const std::string* tool_name = part.FindString("toolName");
    if (!tool_name)
      continue;

    base::DictValue response_obj;
    const auto* result = part.Find("result");
    if (result) {
      response_obj.Set("content", result->Clone());
    }

    base::DictValue func_response;
    func_response.Set("name", *tool_name);
    func_response.Set("response", std::move(response_obj));

    base::DictValue p;
    p.Set("functionResponse", std::move(func_response));
    google_parts.Append(std::move(p));

    // A `functionResponse.response` is opaque JSON the model can't render as
    // an image. When a multimodal tool result carries an image
    // (BrowserScreenshot), also emit it as a real `inlineData` part in this
    // same user turn so the model actually sees the pixels.
    if (const base::ListValue* result_parts = part.FindList("result")) {
      for (const auto& rp_val : *result_parts) {
        const base::DictValue* rp = rp_val.GetIfDict();
        if (!rp)
          continue;
        const std::string* rp_type = rp->FindString("type");
        if (!rp_type || (*rp_type != "image" && *rp_type != "document"))
          continue;
        const std::string* data = *rp_type == "image" ? rp->FindString("image")
                                                       : rp->FindString("data");
        if (!data || data->empty())
          continue;
        const std::string* mime = rp->FindString("mimeType");
        base::DictValue inline_data;
        inline_data.Set("mimeType", mime ? *mime : (*rp_type == "image"
                                                     ? std::string("image/jpeg")
                                                     : std::string("application/pdf")));
        inline_data.Set("data", *data);
        base::DictValue data_part;
        data_part.Set("inlineData", std::move(inline_data));
        google_parts.Append(std::move(data_part));
      }
    }
  }
  return google_parts;
}

void AppendPartsToLastMessage(base::ListValue& contents,
                              const std::string& google_role,
                              base::ListValue& parts) {
  if (!contents.empty() && contents.back().is_dict()) {
    const std::string* prev_role =
        contents.back().GetDict().FindString("role");
    if (prev_role && *prev_role == google_role) {
      auto* prev_parts = contents.back().GetDict().FindList("parts");
      if (prev_parts) {
        for (auto& part : parts) {
          prev_parts->Append(std::move(part));
        }
        return;
      }
    }
  }

  base::DictValue msg;
  msg.Set("role", google_role);
  msg.Set("parts", std::move(parts));
  contents.Append(std::move(msg));
}

base::ListValue TranslateMessages(const base::ListValue& messages,
                                    base::DictValue* system_instruction) {
  base::ListValue contents;
  base::ListValue system_parts;

  for (const auto& msg_value : messages) {
    if (!msg_value.is_dict())
      continue;
    const auto& msg = msg_value.GetDict();
    const std::string* role = msg.FindString("role");
    if (!role)
      continue;

    if (*role == "system") {
      const std::string* text = msg.FindString("content");
      if (text) {
        base::DictValue part;
        part.Set("text", *text);
        system_parts.Append(std::move(part));
      }
      const auto* content_parts = msg.FindList("content");
      if (content_parts) {
        for (const auto& cp : *content_parts) {
          if (!cp.is_dict())
            continue;
          const std::string* cp_type = cp.GetDict().FindString("type");
          const std::string* cp_text = cp.GetDict().FindString("text");
          if (cp_type && *cp_type == "text" && cp_text) {
            base::DictValue part;
            part.Set("text", *cp_text);
            system_parts.Append(std::move(part));
          }
        }
      }
      continue;
    }

    std::string google_role;
    if (*role == "assistant") {
      google_role = "model";
    } else if (*role == "tool") {
      google_role = "user";
    } else {
      google_role = *role;
    }

    base::ListValue parts;

    const std::string* text_content = msg.FindString("content");
    if (text_content) {
      base::DictValue part;
      part.Set("text", *text_content);
      parts.Append(std::move(part));
    }

    const auto* content_parts = msg.FindList("content");
    if (content_parts) {
      if (*role == "tool") {
        parts = TranslateToolResultParts(*content_parts);
      } else if (*role == "assistant") {
        parts = TranslateAssistantContentParts(*content_parts);
      } else {
        parts = TranslateUserContentParts(*content_parts);
      }
    }

    if (parts.empty())
      continue;

    AppendPartsToLastMessage(contents, google_role, parts);
  }

  if (!system_parts.empty()) {
    system_instruction->Set("parts", std::move(system_parts));
  }

  return contents;
}

base::ListValue BuildFunctionDeclarations(const base::ListValue& tools) {
  base::ListValue declarations;
  for (const auto& tool_value : tools) {
    if (!tool_value.is_dict())
      continue;
    // The runtime serializes tools in FLAT shape (see
    // `nexus_llm_host.cc::TranslateBridgeTools` — it emits
    // `{type:"function", name, description, parameters}` per tool). The
    // OpenAI provider re-wraps into `{function:{...}}` in its own
    // `TranslateToolDefinition`; Anthropic reads flat-shape directly. Google
    // used to expect the OpenAI-wrapped layout and walked
    // `GetDict().FindDict("function")`, which never matched the bridge's
    // flat shape — the result was an empty `functionDeclarations` list and
    // Gemini never learned about any tool. Read the flat shape directly.
    const auto& tool = tool_value.GetDict();

    // Skip entries that lack a name — Gemini rejects declarations without
    // one, so it's a cleaner diagnostic to drop them silently than to send
    // a malformed request the real API will reject as a 400.
    const std::string* name = tool.FindString("name");
    if (!name || name->empty())
      continue;

    base::DictValue decl;
    decl.Set("name", *name);
    if (const std::string* description = tool.FindString("description"))
      decl.Set("description", *description);
    if (const auto* parameters = tool.FindDict("parameters"))
      decl.Set("parameters", parameters->Clone());

    declarations.Append(std::move(decl));
  }
  return declarations;
}

base::DictValue BuildToolConfig(const base::Value& tool_choice) {
  base::DictValue calling_config;

  if (tool_choice.is_string()) {
    const std::string& choice = tool_choice.GetString();
    if (choice == "auto") {
      calling_config.Set("mode", "AUTO");
    } else if (choice == "none") {
      calling_config.Set("mode", "NONE");
    } else if (choice == "required") {
      calling_config.Set("mode", "ANY");
    }
  } else if (tool_choice.is_dict()) {
    calling_config.Set("mode", "ANY");
    const auto* function = tool_choice.GetDict().FindDict("function");
    if (function) {
      const std::string* name = function->FindString("name");
      if (name) {
        base::ListValue allowed;
        allowed.Append(*name);
        calling_config.Set("allowedFunctionNames", std::move(allowed));
      }
    }
  }

  base::DictValue config;
  config.Set("functionCallingConfig", std::move(calling_config));
  return config;
}

void ParseCandidateParts(const base::ListValue& parts,
                         std::string& text_out,
                         std::vector<base::DictValue>& tool_calls_out,
                         std::vector<base::DictValue>& reasoning_out) {
  for (const auto& part_value : parts) {
    if (!part_value.is_dict())
      continue;
    const auto& part = part_value.GetDict();

    std::optional<bool> is_thought = part.FindBool("thought");
    const std::string* text = part.FindString("text");

    if (is_thought.value_or(false) && text) {
      base::DictValue reasoning_part;
      reasoning_part.Set("text", *text);
      reasoning_out.push_back(std::move(reasoning_part));
      continue;
    }

    if (text) {
      text_out += *text;
    }

    const auto* func_call = part.FindDict("functionCall");
    if (func_call) {
      base::DictValue tool_call;
      tool_call.Set("toolCallType", "function");
      tool_call.Set("toolCallId",
                    base::Uuid::GenerateRandomV4().AsLowercaseString());

      const std::string* name = func_call->FindString("name");
      if (name) {
        tool_call.Set("toolName", *name);
      }

      const auto* args = func_call->FindDict("args");
      if (args) {
        std::string args_str;
        base::JSONWriter::Write(base::Value(args->Clone()), &args_str);
        tool_call.Set("args", args_str);
      }

      tool_calls_out.push_back(std::move(tool_call));
    }
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// StreamHandler
// ---------------------------------------------------------------------------

class GoogleProvider::StreamHandler
    : public network::SimpleURLLoaderStreamConsumer {
 public:
  StreamHandler(std::unique_ptr<network::SimpleURLLoader> loader,
                StreamChunkCallback chunk_callback,
                StreamDoneCallback done_callback,
                base::OnceClosure finished_closure)
      : loader_(std::move(loader)),
        chunk_callback_(std::move(chunk_callback)),
        done_callback_(std::move(done_callback)),
        finished_closure_(std::move(finished_closure)) {}

  ~StreamHandler() override = default;

  StreamHandler(const StreamHandler&) = delete;
  StreamHandler& operator=(const StreamHandler&) = delete;

  void Start(
      scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory) {
    loader_->DownloadAsStream(url_loader_factory.get(), this);
  }

  // network::SimpleURLLoaderStreamConsumer:
  void OnDataReceived(std::string_view string_piece,
                      base::OnceClosure resume) override {
    buffer_.append(string_piece);
    ProcessBuffer();
    std::move(resume).Run();
  }

  void OnComplete(bool success) override {
    ProcessBuffer();

    if (!success) {
      int net_error = loader_->NetError();
      std::move(done_callback_)
          .Run(base::StrCat({"Stream failed with network error ",
                             base::NumberToString(net_error)}));
      // May destroy |this| — must be the last statement.
      if (finished_closure_)
        std::move(finished_closure_).Run();
      return;
    }

    base::DictValue finish_chunk;
    finish_chunk.Set("type", "finish");
    finish_chunk.Set("finishReason", last_finish_reason_);

    base::DictValue usage;
    // Gemini's promptTokenCount is INCLUSIVE of cachedContentTokenCount; emit
    // input EXCLUSIVE of cache to match the bridge's Anthropic convention
    // (the JS token counter sums input + cache_read + cache_creation + output,
    // so leaving cache in the prompt count would double-count).
    usage.Set("promptTokens", prompt_tokens_ > cached_content_tokens_
                                  ? prompt_tokens_ - cached_content_tokens_
                                  : 0);
    usage.Set("completionTokens", completion_tokens_);
    usage.Set("cacheReadInputTokens", cached_content_tokens_);
    finish_chunk.Set("usage", std::move(usage));

    chunk_callback_.Run(std::move(finish_chunk));
    std::move(done_callback_).Run(std::nullopt);
    // May destroy |this| — must be the last statement.
    if (finished_closure_)
      std::move(finished_closure_).Run();
  }

  void OnRetry(base::OnceClosure start_retry) override {
    std::move(start_retry).Run();
  }

 private:
  void ProcessBuffer() {
    while (true) {
      size_t newline_pos = buffer_.find('\n');
      if (newline_pos == std::string::npos)
        break;

      std::string line = buffer_.substr(0, newline_pos);
      buffer_.erase(0, newline_pos + 1);

      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      if (line.empty())
        continue;

      std::string_view data;
      if (line.starts_with("data: ")) {
        data = std::string_view(line).substr(6);
      } else if (line.starts_with("data:")) {
        data = std::string_view(line).substr(5);
      } else {
        continue;
      }

      if (data == "[DONE]")
        continue;

      ProcessSsePayload(data);
    }
  }

  void ProcessSsePayload(std::string_view json_str) {
    auto parsed =
        base::JSONReader::ReadDict(json_str, base::JSON_PARSE_RFC);
    if (!parsed) {
      DLOG(ERROR) << "Google SSE: failed to parse JSON payload";
      return;
    }

    const auto* candidates = parsed->FindList("candidates");
    if (!candidates || candidates->empty())
      return;

    const auto& candidate = (*candidates)[0];
    if (!candidate.is_dict())
      return;

    const auto* content = candidate.GetDict().FindDict("content");
    if (content) {
      const auto* parts = content->FindList("parts");
      if (parts) {
        EmitStreamParts(*parts);
      }
    }

    const std::string* finish_reason =
        candidate.GetDict().FindString("finishReason");
    if (finish_reason) {
      last_finish_reason_ = *finish_reason;
    }

    const auto* usage = parsed->FindDict("usageMetadata");
    if (usage) {
      if (auto pt = usage->FindInt("promptTokenCount"))
        prompt_tokens_ = *pt;
      if (auto ct = usage->FindInt("candidatesTokenCount"))
        completion_tokens_ = *ct;
      // Gemini reports cached prompt tokens under `cachedContentTokenCount`.
      if (auto cached = usage->FindInt("cachedContentTokenCount"))
        cached_content_tokens_ = *cached;
    }
  }

  void EmitStreamParts(const base::ListValue& parts) {
    for (const auto& part_value : parts) {
      if (!part_value.is_dict())
        continue;
      const auto& part = part_value.GetDict();

      std::optional<bool> is_thought = part.FindBool("thought");
      const std::string* text = part.FindString("text");

      if (is_thought.value_or(false) && text && !text->empty()) {
        base::DictValue chunk;
        chunk.Set("type", "reasoning");
        chunk.Set("text", *text);
        chunk_callback_.Run(std::move(chunk));
        continue;
      }

      if (text && !text->empty()) {
        base::DictValue chunk;
        chunk.Set("type", "text-delta");
        chunk.Set("textDelta", *text);
        chunk_callback_.Run(std::move(chunk));
      }

      const auto* func_call = part.FindDict("functionCall");
      if (func_call) {
        base::DictValue chunk;
        chunk.Set("type", "tool-call");
        chunk.Set("toolCallType", "function");
        chunk.Set(
            "toolCallId",
            base::Uuid::GenerateRandomV4().AsLowercaseString());

        const std::string* name = func_call->FindString("name");
        if (name)
          chunk.Set("toolName", *name);

        const auto* args = func_call->FindDict("args");
        if (args) {
          std::string args_str;
          base::JSONWriter::Write(base::Value(args->Clone()), &args_str);
          chunk.Set("args", args_str);
        }

        chunk_callback_.Run(std::move(chunk));
      }
    }
  }

  std::unique_ptr<network::SimpleURLLoader> loader_;
  StreamChunkCallback chunk_callback_;
  StreamDoneCallback done_callback_;
  // Notifies the owning GoogleProvider that this stream is finished so the
  // pending_streams_ entry can be erased. Posted (not synchronous) — see
  // DoStream. Without this the handler + loader leaked in pending_streams_
  // for the lifetime of the provider after every completed stream.
  base::OnceClosure finished_closure_;
  std::string buffer_;
  std::string last_finish_reason_;
  int cached_content_tokens_ = 0;
  int prompt_tokens_ = 0;
  int completion_tokens_ = 0;
};

GoogleProvider::PendingGenerate::PendingGenerate() = default;
GoogleProvider::PendingGenerate::~PendingGenerate() = default;
GoogleProvider::PendingGenerate::PendingGenerate(PendingGenerate&&) = default;
GoogleProvider::PendingGenerate& GoogleProvider::PendingGenerate::operator=(
    PendingGenerate&&) = default;

// ---------------------------------------------------------------------------
// GoogleProvider
// ---------------------------------------------------------------------------

GoogleProvider::GoogleProvider(
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory)
    : url_loader_factory_(std::move(url_loader_factory)) {
  DETACH_FROM_SEQUENCE(sequence_checker_);
}

GoogleProvider::~GoogleProvider() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

std::string GoogleProvider::GetId() const {
  return "google";
}

std::string GoogleProvider::GetDisplayName() const {
  return "Google AI";
}

LlmProviderType GoogleProvider::GetProviderType() const {
  return LlmProviderType::kCloud;
}

std::vector<ModelInfo> GoogleProvider::GetSupportedModels() const {
  return fetched_models_;
}

std::string GoogleProvider::GetDefaultModelId() const {
  // Default-model selection is owned by the TS layer (nexus_web).
  return std::string();
}

bool GoogleProvider::SupportsStreaming() const {
  return true;
}

bool GoogleProvider::SupportsTools() const {
  return true;
}

bool GoogleProvider::IsConfigured() const {
  return !api_key_.empty();
}

bool GoogleProvider::Configure(const base::DictValue& config) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  ConfigureUserAgent(config);

  const std::string* api_key = config.FindString("api_key");
  if (!api_key || api_key->empty()) {
    DLOG(ERROR) << "Google AI provider requires a non-empty api_key";
    return false;
  }
  api_key_ = NexusSecretCrypt::GetInstance()->Decrypt(*api_key);
  // A non-empty stored value that decrypts to nothing is an UNOPENABLE
  // ciphertext (settings.json copied to another machine / OS user, keystore
  // reset, or the encryptor not ready yet). Report "not configured" rather
  // than sending every request with an empty key — mirrors OpenAI/Anthropic,
  // whose Configure returns IsConfigured().
  if (api_key_.empty()) {
    LOG(ERROR) << "Google AI provider: the stored api_key could not be "
                  "decrypted; treating the provider as unconfigured";
    return false;
  }

  const std::string* model = config.FindString("model");
  if (model && !model->empty()) {
    model_id_ = *model;
  }

  // A non-empty base_url is a user override; an empty one means "use the
  // default" and drops a previous override. Absent leaves it unchanged.
  if (const std::string* base_url = config.FindString("base_url")) {
    base_url_overridden_ = !base_url->empty();
    base_url_ = base_url_overridden_ ? *base_url : default_base_url_;
  }

  return true;
}

std::string GoogleProvider::GetDefaultBaseUrl() const {
  return default_base_url_;
}

void GoogleProvider::SetDefaultBaseUrl(const std::string& url) {
  default_base_url_ = url;
  if (!base_url_overridden_) {
    base_url_ = default_base_url_;
  }
}

std::string GoogleProvider::GetBaseUrl() const {
  return base_url_;
}

GURL GoogleProvider::BuildEndpointUrl(const std::string& model_id,
                                      bool streaming) const {
  std::string api_base = base_url_;
  if (!api_base.empty() && api_base.back() == '/') {
    api_base.pop_back();
  }

  if (streaming) {
    return GURL(base::StrCat(
        {api_base, "/models/", model_id,
         ":streamGenerateContent?alt=sse&key=", api_key_}));
  }
  return GURL(
      base::StrCat(
          {api_base, "/models/", model_id, ":generateContent?key=", api_key_}));
}

base::DictValue GoogleProvider::BuildRequestBody(
    const base::DictValue& call_options) const {
  base::DictValue body;

  const auto* messages = call_options.FindList("messages");
  if (messages) {
    base::DictValue system_instruction;
    base::ListValue contents =
        TranslateMessages(*messages, &system_instruction);

    if (!system_instruction.empty()) {
      body.Set("systemInstruction", std::move(system_instruction));
    }
    if (!contents.empty()) {
      body.Set("contents", std::move(contents));
    }
  }

  const auto* tools = call_options.FindList("tools");
  if (tools && !tools->empty()) {
    base::ListValue declarations = BuildFunctionDeclarations(*tools);
    if (!declarations.empty()) {
      base::DictValue tools_wrapper;
      tools_wrapper.Set("functionDeclarations", std::move(declarations));

      base::ListValue tools_array;
      tools_array.Append(std::move(tools_wrapper));
      body.Set("tools", std::move(tools_array));
    }

    const auto* tool_choice = call_options.Find("tool_choice");
    if (tool_choice) {
      body.Set("toolConfig", BuildToolConfig(*tool_choice));
    }
  }

  base::DictValue gen_config;
  if (auto max_tokens = call_options.FindInt("max_tokens")) {
    gen_config.Set("maxOutputTokens", *max_tokens);
  }
  if (auto temperature = call_options.FindDouble("temperature")) {
    gen_config.Set("temperature", *temperature);
  }
  if (auto top_p = call_options.FindDouble("top_p")) {
    gen_config.Set("topP", *top_p);
  }
  if (auto top_k = call_options.FindInt("top_k")) {
    gen_config.Set("topK", *top_k);
  }
  const auto* stop = call_options.FindList("stop");
  if (stop && !stop->empty()) {
    gen_config.Set("stopSequences", stop->Clone());
  }
  if (!gen_config.empty()) {
    body.Set("generationConfig", std::move(gen_config));
  }

  // Merge TS-built provider-specific fields last so they override base params.
  if (const auto* ext = call_options.FindDict("providerBodyExtension")) {
    for (auto [key, val] : *ext) {
      body.Set(key, val.Clone());
    }
  }

  return body;
}

GenerateResult GoogleProvider::ParseResponse(
    const base::DictValue& response) const {
  GenerateResult result;

  const auto* candidates = response.FindList("candidates");
  if (!candidates || candidates->empty()) {
    const auto* prompt_feedback = response.FindDict("promptFeedback");
    if (prompt_feedback) {
      const std::string* block_reason =
          prompt_feedback->FindString("blockReason");
      if (block_reason && !block_reason->empty()) {
        result.finish_reason = FinishReason::kContentFilter;
        return result;
      }
    }
    result.finish_reason = FinishReason::kError;
    return result;
  }

  const auto& first_candidate = (*candidates)[0];
  if (!first_candidate.is_dict()) {
    result.finish_reason = FinishReason::kError;
    return result;
  }
  const auto& candidate = first_candidate.GetDict();

  const std::string* finish_reason_str = candidate.FindString("finishReason");
  if (finish_reason_str) {
    result.finish_reason = MapGoogleFinishReason(*finish_reason_str);
  }

  const auto* content = candidate.FindDict("content");
  if (content) {
    const auto* parts = content->FindList("parts");
    if (parts) {
      std::string text_content;
      std::vector<base::DictValue> tool_calls;
      std::vector<base::DictValue> reasoning;

      ParseCandidateParts(*parts, text_content, tool_calls, reasoning);

      if (!text_content.empty()) {
        result.text = std::move(text_content);
      }
      if (!tool_calls.empty()) {
        result.tool_calls = std::move(tool_calls);
        if (result.finish_reason == FinishReason::kStop ||
            result.finish_reason == FinishReason::kUnknown) {
          result.finish_reason = FinishReason::kToolCalls;
        }
      }
      if (!reasoning.empty()) {
        result.reasoning = std::move(reasoning);
      }
    }
  }

  const auto* usage = response.FindDict("usageMetadata");
  if (usage) {
    if (auto ct = usage->FindInt("candidatesTokenCount"))
      result.completion_tokens = *ct;
    const int cached = usage->FindInt("cachedContentTokenCount").value_or(0);
    result.cache_read_input_tokens = cached;
    // promptTokenCount is INCLUSIVE of cachedContentTokenCount; store input
    // EXCLUSIVE of cache (Anthropic convention used by the bridge + JS sum).
    const int prompt = usage->FindInt("promptTokenCount").value_or(0);
    result.prompt_tokens = prompt > cached ? prompt - cached : 0;
  }

  return result;
}

std::unique_ptr<network::SimpleURLLoader> GoogleProvider::CreateLoader(
    const GURL& url,
    const std::string& body) const {
  auto resource_request = std::make_unique<network::ResourceRequest>();
  resource_request->url = url;
  resource_request->method = "POST";
  ApplyUserAgent(resource_request->headers);
  resource_request->credentials_mode =
      network::mojom::CredentialsMode::kOmit;

  auto loader =
      network::SimpleURLLoader::Create(std::move(resource_request),
                                       kTrafficAnnotation);
  loader->AttachStringForUpload(body, "application/json");
  return loader;
}

void GoogleProvider::DoGenerate(const base::DictValue& call_options,
                                GenerateCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (!IsConfigured()) {
    GenerateResult result;
    result.finish_reason = FinishReason::kError;
    std::move(callback).Run(std::move(result),
                            "Google AI provider is not configured");
    return;
  }

  // Model selection is owned by the TS layer. If neither the call nor the
  // stored config provides one, the request ships with an empty model
  // segment and the API will reject it — that is the correct fail-loud.
  std::string model = model_id_;
  if (const auto* model_override = call_options.FindString("model")) {
    model = *model_override;
  }

  base::DictValue request_body = BuildRequestBody(call_options);
  std::string body_json;
  base::JSONWriter::Write(base::Value(request_body.Clone()), &body_json);

  GURL url = BuildEndpointUrl(model, /*streaming=*/false);
  auto loader = CreateLoader(url, body_json);

  std::string request_id = ResolveRequestId(call_options);

  auto* loader_ptr = loader.get();
  auto& pending = pending_generates_[request_id];
  pending.loader = std::move(loader);
  pending.callback = std::move(callback);
  pending.request_body = std::move(request_body);

  loader_ptr->DownloadToString(
      url_loader_factory_.get(),
      base::BindOnce(&GoogleProvider::OnGenerateComplete,
                     weak_factory_.GetWeakPtr(), request_id),
      kMaxResponseSizeBytes);
}

void GoogleProvider::OnGenerateComplete(
    const std::string& request_id,
    std::optional<std::string> response_body) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  auto it = pending_generates_.find(request_id);
  if (it == pending_generates_.end())
    return;

  auto callback = std::move(it->second.callback);
  auto raw_call = std::move(it->second.request_body);
  int net_error = it->second.loader->NetError();
  pending_generates_.erase(it);

  if (!response_body) {
    GenerateResult result;
    result.finish_reason = FinishReason::kError;
    result.raw_call = std::move(raw_call);
    std::move(callback).Run(
        std::move(result),
        base::StrCat(
            {"Network error: ", base::NumberToString(net_error)}));
    return;
  }

  auto parsed =
      base::JSONReader::ReadDict(*response_body, base::JSON_PARSE_RFC);
  if (!parsed) {
    GenerateResult result;
    result.finish_reason = FinishReason::kError;
    result.raw_call = std::move(raw_call);
    // The shared extractor still recovers a message from SSE-framed or
    // gateway-wrapped error bodies that don't parse as a single JSON
    // object; only fall back to the generic message when nothing
    // recognizable is there.
    std::string error = ExtractApiErrorMessage(*response_body);
    if (error.empty()) {
      error = "Failed to parse Google AI response JSON";
    }
    std::move(callback).Run(std::move(result), std::move(error));
    return;
  }

  const auto* error = parsed->FindDict("error");
  if (error) {
    const std::string* message = error->FindString("message");
    GenerateResult result;
    result.finish_reason = FinishReason::kError;
    result.raw_call = std::move(raw_call);
    result.raw_response = std::move(*parsed);
    std::move(callback).Run(
        std::move(result),
        message ? *message : "Unknown Google AI API error");
    return;
  }

  GenerateResult result = ParseResponse(*parsed);
  result.raw_call = std::move(raw_call);
  result.raw_response = std::move(*parsed);
  std::move(callback).Run(std::move(result), std::nullopt);
}

void GoogleProvider::DoStream(const base::DictValue& call_options,
                              StreamChunkCallback chunk_callback,
                              StreamDoneCallback done_callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (!IsConfigured()) {
    std::move(done_callback).Run("Google AI provider is not configured");
    return;
  }

  // Model selection is owned by the TS layer. If neither the call nor the
  // stored config provides one, the request ships with an empty model
  // segment and the API will reject it — that is the correct fail-loud.
  std::string model = model_id_;
  if (const auto* model_override = call_options.FindString("model")) {
    model = *model_override;
  }

  base::DictValue request_body = BuildRequestBody(call_options);
  std::string body_json;
  base::JSONWriter::Write(base::Value(request_body.Clone()), &body_json);

  GURL url = BuildEndpointUrl(model, /*streaming=*/true);
  auto loader = CreateLoader(url, body_json);

  std::string request_id = ResolveRequestId(call_options);

  // Wire chunk/done through BindPostTask: the host/guest chain behind them
  // can call back into CancelRequest synchronously, which erases
  // pending_streams_ and destroys the StreamHandler while it is still
  // executing (ProcessBuffer / EmitStreamParts) — a use-after-free. Posting
  // breaks the re-entrancy; same-sequence posting preserves chunk→done order.
  // The finished closure is posted too, so erasing the handler never happens
  // from inside its own OnComplete stack frame.
  auto task_runner = base::SequencedTaskRunner::GetCurrentDefault();
  auto handler = std::make_unique<StreamHandler>(
      std::move(loader),
      base::BindPostTask(task_runner, std::move(chunk_callback)),
      base::BindPostTask(task_runner, std::move(done_callback)),
      base::BindPostTask(
          task_runner,
          base::BindOnce(&GoogleProvider::OnStreamFinished,
                         weak_factory_.GetWeakPtr(), request_id)));
  auto* handler_ptr = handler.get();
  pending_streams_[request_id] = std::move(handler);

  handler_ptr->Start(url_loader_factory_);
}

void GoogleProvider::CancelRequest(const std::string& request_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  pending_generates_.erase(request_id);
  pending_streams_.erase(request_id);
}

void GoogleProvider::OnStreamFinished(const std::string& request_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  pending_streams_.erase(request_id);
}

void GoogleProvider::FetchAvailableModels(FetchModelsCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (api_key_.empty()) {
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
      [](base::WeakPtr<GoogleProvider> self, FetchModelsCallback primary,
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

  std::string api_base = base_url_;
  if (!api_base.empty() && api_base.back() == '/') {
    api_base.pop_back();
  }

  GURL url(base::StrCat({api_base, "/models?key=", api_key_}));

  auto resource_request = std::make_unique<network::ResourceRequest>();
  resource_request->url = url;
  resource_request->method = "GET";
  ApplyUserAgent(resource_request->headers);
  resource_request->credentials_mode =
      network::mojom::CredentialsMode::kOmit;

  models_loader_ =
      network::SimpleURLLoader::Create(std::move(resource_request),
                                       kTrafficAnnotation);

  models_loader_->DownloadToString(
      url_loader_factory_.get(),
      base::BindOnce(&GoogleProvider::OnFetchModelsComplete,
                     weak_factory_.GetWeakPtr(), std::move(callback)),
      kMaxResponseSizeBytes);
}

void GoogleProvider::OnFetchModelsComplete(
    FetchModelsCallback callback,
    std::optional<std::string> response_body) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  models_loader_.reset();

  // Capability flags and filtering (e.g. excluding embedding models) all
  // live in the TS layer. The native provider passes through whatever the
  // Google /models endpoint returned. We still strip the "models/" prefix
  // because that is wire-format normalisation, not model identity.
  if (!response_body) {
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
    std::move(callback).Run({});
    return;
  }

  const auto* models_list = parsed->FindList("models");
  if (!models_list) {
    std::move(callback).Run({});
    return;
  }

  std::vector<ModelInfo> models;
  for (const auto& model_val : *models_list) {
    const base::DictValue* model = model_val.GetIfDict();
    if (!model)
      continue;

    const std::string* name_str = model->FindString("name");
    if (!name_str || name_str->empty())
      continue;

    std::string id = *name_str;
    if (base::StartsWith(id, "models/"))
      id = id.substr(7);

    const std::string* display_name = model->FindString("displayName");
    std::string model_name = display_name && !display_name->empty()
                                ? *display_name
                                : id;

    ModelInfo info;
    info.id = std::move(id);
    info.name = std::move(model_name);
    models.push_back(std::move(info));
  }

  fetched_models_ = models;
  std::move(callback).Run(std::move(models));
}

void GoogleProvider::LoadCachedCatalogJson(const std::string& raw_json) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (raw_json.empty())
    return;
  OnFetchModelsComplete(base::DoNothing(), raw_json);
}

std::string GoogleProvider::GetLastFetchedRawCatalogJson() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return last_raw_catalog_json_;
}

}  // namespace nexus
