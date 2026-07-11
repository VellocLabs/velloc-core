#include "custom_browser/browser/nexus/api/llm/llm_types.h"

namespace nexus {

ProviderInfo::ProviderInfo() = default;
ProviderInfo::ProviderInfo(const ProviderInfo&) = default;
ProviderInfo& ProviderInfo::operator=(const ProviderInfo&) = default;
ProviderInfo::~ProviderInfo() = default;

ToolDefinition::ToolDefinition() = default;
ToolDefinition::ToolDefinition(ToolDefinition&&) = default;
ToolDefinition& ToolDefinition::operator=(ToolDefinition&&) = default;
ToolDefinition::~ToolDefinition() = default;

ToolChoice::ToolChoice() = default;
ToolChoice::ToolChoice(const ToolChoice&) = default;
ToolChoice& ToolChoice::operator=(const ToolChoice&) = default;
ToolChoice::~ToolChoice() = default;

ToolCallResult::ToolCallResult() = default;
ToolCallResult::ToolCallResult(const ToolCallResult&) = default;
ToolCallResult& ToolCallResult::operator=(const ToolCallResult&) = default;
ToolCallResult::~ToolCallResult() = default;

ReasoningPart::ReasoningPart() = default;
ReasoningPart::ReasoningPart(const ReasoningPart&) = default;
ReasoningPart& ReasoningPart::operator=(const ReasoningPart&) = default;
ReasoningPart::~ReasoningPart() = default;

ContentPart::ContentPart() = default;
ContentPart::ContentPart(ContentPart&&) = default;
ContentPart& ContentPart::operator=(ContentPart&&) = default;
ContentPart::~ContentPart() = default;

PromptMessage::PromptMessage() = default;
PromptMessage::PromptMessage(PromptMessage&&) = default;
PromptMessage& PromptMessage::operator=(PromptMessage&&) = default;
PromptMessage::~PromptMessage() = default;

GenerationMode::GenerationMode() = default;
GenerationMode::GenerationMode(GenerationMode&&) = default;
GenerationMode& GenerationMode::operator=(GenerationMode&&) = default;
GenerationMode::~GenerationMode() = default;

CallOptions::CallOptions() = default;
CallOptions::CallOptions(CallOptions&&) = default;
CallOptions& CallOptions::operator=(CallOptions&&) = default;
CallOptions::~CallOptions() = default;

GenerateResult::GenerateResult() = default;
GenerateResult::GenerateResult(GenerateResult&&) = default;
GenerateResult& GenerateResult::operator=(GenerateResult&&) = default;
GenerateResult::~GenerateResult() = default;

StreamPart::StreamPart() = default;
StreamPart::StreamPart(const StreamPart&) = default;
StreamPart& StreamPart::operator=(const StreamPart&) = default;
StreamPart::~StreamPart() = default;

FinishReason TranslateOpenAiFinishReason(std::string_view reason) {
  if (reason == "stop")
    return FinishReason::kStop;
  if (reason == "length")
    return FinishReason::kLength;
  if (reason == "content_filter")
    return FinishReason::kContentFilter;
  if (reason == "tool_calls")
    return FinishReason::kToolCalls;
  return FinishReason::kOther;
}

}  // namespace nexus
