#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_LLM_LLM_TYPES_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_LLM_LLM_TYPES_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/functional/callback.h"
#include "base/values.h"

namespace nexus {

// =============================================================================
// Enums
// =============================================================================

// Provider classification.
enum class LlmProviderType {
  kCloud,
  kLocal,
  kProxy,
};

// Mirrors Vercel's LanguageModelV3FinishReason.
enum class FinishReason {
  kStop,
  kLength,
  kContentFilter,
  kToolCalls,
  kError,
  kOther,
  kUnknown,
};

// Mirrors Vercel's LanguageModelV3Message role.
enum class PromptRole {
  kSystem,
  kUser,
  kAssistant,
  kTool,
};

// =============================================================================
// Provider metadata
// =============================================================================

// Metadata for a single model offered by a provider.
struct ModelInfo {
  std::string id;
  std::string name;
  int context_window = 0;
  bool supports_tools = false;
  bool supports_vision = false;
};

// Metadata for a registered provider, including its model catalogue.
struct ProviderInfo {
  ProviderInfo();
  ProviderInfo(const ProviderInfo&);
  ProviderInfo& operator=(const ProviderInfo&);
  ~ProviderInfo();

  std::string id;
  std::string name;
  LlmProviderType type = LlmProviderType::kCloud;
  bool is_configured = false;
  std::string base_url;
  std::string default_base_url;
  std::vector<ModelInfo> models;
};

// =============================================================================
// Wire format types — aligned with Vercel AI SDK v6 LanguageModelV3
//
// These struct definitions are the canonical C++ representation of the JSON
// wire format flowing over the postMessage bridge. The TypeScript bridge passes
// LanguageModelV3CallOptions through with near-zero translation; the C++
// broker and providers parse base::DictValue representations of these types.
// =============================================================================

// Mirrors Vercel's LanguageModelV3Usage.
struct Usage {
  int prompt_tokens = 0;
  int completion_tokens = 0;
  int cache_read_input_tokens = 0;
  int cache_creation_input_tokens = 0;
};

// Mirrors Vercel's LanguageModelV3FunctionTool.
struct ToolDefinition {
  ToolDefinition();
  ToolDefinition(ToolDefinition&&);
  ToolDefinition& operator=(ToolDefinition&&);
  ~ToolDefinition();

  std::string type;  // Always "function"
  std::string name;
  std::optional<std::string> description;
  base::DictValue parameters;  // JSON Schema for tool input
};

// Mirrors Vercel's LanguageModelV3ToolChoice. Discriminator: |type|.
//   "auto" / "none" / "required" — simple mode, |tool_name| unused.
//   "tool" — forces a specific tool, |tool_name| is set.
struct ToolChoice {
  ToolChoice();
  ToolChoice(const ToolChoice&);
  ToolChoice& operator=(const ToolChoice&);
  ~ToolChoice();

  std::string type;  // "auto", "none", "required", or "tool"
  std::optional<std::string> tool_name;
};

// Mirrors Vercel's LanguageModelV3ToolCallDelta (completed form).
struct ToolCallResult {
  ToolCallResult();
  ToolCallResult(const ToolCallResult&);
  ToolCallResult& operator=(const ToolCallResult&);
  ~ToolCallResult();

  std::string tool_call_type;  // Always "function"
  std::string tool_call_id;
  std::string tool_name;
  std::string args;  // JSON string of tool arguments
};

// Reasoning / thinking content produced by models that support it.
struct ReasoningPart {
  ReasoningPart();
  ReasoningPart(const ReasoningPart&);
  ReasoningPart& operator=(const ReasoningPart&);
  ~ReasoningPart();

  std::string type;  // Always "text"
  std::string text;
  std::optional<std::string> signature;
};

// Content part within a PromptMessage. Discriminator: |type|.
//   "text"        — |text| is set.
//   "image"       — |image| and |mime_type| are set.
//   "tool-call"   — |tool_call_id|, |tool_name|, |args| are set.
//   "tool-result" — |tool_call_id|, |tool_name|, |result| are set.
struct ContentPart {
  ContentPart();
  ContentPart(ContentPart&&);
  ContentPart& operator=(ContentPart&&);
  ~ContentPart();

  std::string type;

  // "text"
  std::optional<std::string> text;

  // "image"
  std::optional<std::string> image;  // Base64 data or URL
  std::optional<std::string> mime_type;

  // "tool-call" / "tool-result" shared
  std::optional<std::string> tool_call_id;
  std::optional<std::string> tool_name;

  // "tool-call"
  std::optional<base::DictValue> args;

  // "tool-result"
  std::optional<base::Value> result;
  std::optional<bool> is_error;
};

// A single message in the prompt. Mirrors Vercel's LanguageModelV3Message.
struct PromptMessage {
  PromptMessage();
  PromptMessage(PromptMessage&&);
  PromptMessage& operator=(PromptMessage&&);
  ~PromptMessage();

  PromptRole role = PromptRole::kSystem;
  std::vector<ContentPart> content;
  std::optional<base::DictValue> provider_metadata;
};

// Generation mode. Discriminator: |type|.
//   "regular"     — standard generation with optional tools/toolChoice.
//   "object-json" — structured JSON output with optional schema.
//   "object-tool" — structured output via a specific tool.
struct GenerationMode {
  GenerationMode();
  GenerationMode(GenerationMode&&);
  GenerationMode& operator=(GenerationMode&&);
  ~GenerationMode();

  std::string type;  // "regular", "object-json", "object-tool"

  // "regular"
  std::optional<std::vector<ToolDefinition>> tools;
  std::optional<ToolChoice> tool_choice;

  // "object-json"
  std::optional<base::DictValue> schema;
  std::optional<std::string> schema_name;
  std::optional<std::string> schema_description;

  // "object-tool"
  std::optional<ToolDefinition> tool;
};

// Input to DoGenerate/DoStream. Mirrors Vercel's LanguageModelV3CallOptions.
struct CallOptions {
  CallOptions();
  CallOptions(CallOptions&&);
  CallOptions& operator=(CallOptions&&);
  ~CallOptions();

  std::string request_id;
  std::string model_id;
  std::string input_format;  // "prompt" or "messages"
  std::vector<PromptMessage> prompt;
  GenerationMode mode;
  std::optional<int> max_tokens;
  std::optional<double> temperature;
  std::optional<double> top_p;
  std::optional<int> top_k;
  std::optional<double> presence_penalty;
  std::optional<double> frequency_penalty;
  std::optional<std::vector<std::string>> stop_sequences;
  std::optional<int> seed;
  std::optional<base::DictValue> response_format;
  std::optional<base::DictValue> provider_options;
};

// =============================================================================
// Generation result (output)
// =============================================================================

// Output of DoGenerate. Mirrors LanguageModelV3GenerateResult.
// Fields are Vercel-aligned — returned as-is over the postMessage bridge.
struct GenerateResult {
  GenerateResult();
  GenerateResult(GenerateResult&&);
  GenerateResult& operator=(GenerateResult&&);
  ~GenerateResult();

  std::optional<std::string> text;
  std::optional<std::vector<base::DictValue>> reasoning;
  std::optional<std::vector<base::DictValue>> tool_calls;
  FinishReason finish_reason = FinishReason::kUnknown;
  int prompt_tokens = 0;
  int completion_tokens = 0;
  int cache_read_input_tokens = 0;
  int cache_creation_input_tokens = 0;
  std::optional<base::DictValue> raw_call;
  std::optional<base::DictValue> raw_response;
  std::optional<std::vector<base::DictValue>> warnings;
  std::optional<base::DictValue> provider_metadata;
};

// =============================================================================
// Streaming result (output)
// =============================================================================

// A single chunk in a streaming response. Mirrors Vercel's
// LanguageModelV3StreamPart discriminated union. Discriminator: |type|.
//   "text-delta"      — |text_delta| is set.
//   "reasoning-delta" — |text_delta| is set.
//   "tool-call-delta" — partial tool call: |tool_call_type|, |tool_call_id|,
//                        |tool_name|, |args_text_delta|.
//   "tool-call"       — completed tool call: same fields + |args|.
//   "finish"          — |finish_reason| and |usage| are set.
//   "error"           — |error| is set.
struct StreamPart {
  StreamPart();
  StreamPart(const StreamPart&);
  StreamPart& operator=(const StreamPart&);
  ~StreamPart();

  std::string type;

  // "text-delta" / "reasoning-delta"
  std::optional<std::string> text_delta;

  // "tool-call-delta" / "tool-call" shared
  std::optional<std::string> tool_call_type;
  std::optional<std::string> tool_call_id;
  std::optional<std::string> tool_name;

  // "tool-call-delta"
  std::optional<std::string> args_text_delta;

  // "tool-call" (completed)
  std::optional<std::string> args;

  // "finish"
  std::optional<FinishReason> finish_reason;
  std::optional<Usage> usage;

  // "error"
  std::optional<std::string> error;
};

// =============================================================================
// Callbacks
// =============================================================================

// Non-streaming completion callback.
using GenerateCallback =
    base::OnceCallback<void(GenerateResult result,
                            std::optional<std::string> error)>;

// Streaming: called once per chunk (base::DictValue with "type" discriminator).
using StreamChunkCallback =
    base::RepeatingCallback<void(base::DictValue chunk)>;

// Streaming: called when the stream ends (nullopt on success, error message on
// failure).
using StreamDoneCallback =
    base::OnceCallback<void(std::optional<std::string> error)>;

// Called when a dynamic model-list fetch completes.
using FetchModelsCallback =
    base::OnceCallback<void(std::vector<ModelInfo> models)>;

// =============================================================================
// FinishReason helpers
// =============================================================================

// Translate an OpenAI-style finish_reason string into the enum. Used by
// providers that speak OpenAI's chat-completions protocol (OpenAI, Ollama,
// and the OpenAI-compatible providers). Returns kOther for unknown values.
FinishReason TranslateOpenAiFinishReason(std::string_view reason);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_LLM_LLM_TYPES_H_
