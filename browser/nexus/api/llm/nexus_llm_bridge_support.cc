#include "custom_browser/browser/nexus/api/llm/nexus_llm_bridge_support.h"

#include <algorithm>
#include <utility>

#include "base/check.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/numerics/safe_conversions.h"
#include "base/uuid.h"
#include "custom_browser/browser/nexus/api/catalog/prompt_holder_registry.h"
#include "custom_browser/browser/nexus/mojom/nexus_llm.mojom.h"
#include "custom_browser/browser/nexus/nexus_bridge_support.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_schema.h"
#include "custom_browser/browser/nexus/tools/tool_catalog.h"

namespace nexus {

namespace {

using base::Value;
using Dict = base::DictValue;
using List = base::ListValue;

// Resolve a tool `{ ref: "<WireName>" }` to the catalog's full definition
// dict (name + description + parameters + parametersStrict + inputExamples).
// nexus_web ships host browser/file/process tools as refs under symbolic
// emission; the LLM module fills the whole definition here so the per-turn
// schema never crosses the wire. Returns nullopt for an unknown name (the
// entry is then skipped, matching the JS resolver contract). The dict
// carries BOTH parameter variants; `ConvertBridgeTools` selects strict-vs-
// regular per the request's `strictTools`.
std::optional<Dict> ResolveToolRefToDict(const std::string& ref_name) {
  for (const ToolSchema& schema : ToolCatalog::Get().schemas()) {
    if (schema.name == ref_name) {
      return BuildHostedToolDefinitionDict(schema);
    }
  }
  return std::nullopt;
}

void AppendFormattedText(const Value& value, std::vector<std::string>* out) {
  if (const std::string* text = value.GetIfString(); text && !text->empty()) {
    out->push_back(*text);
    return;
  }

  if (const List* list = value.GetIfList()) {
    for (const Value& entry : *list) {
      AppendFormattedText(entry, out);
    }
    return;
  }

  if (const Dict* dict = value.GetIfDict()) {
    if (const std::string* text = dict->FindString("text");
        text && !text->empty()) {
      out->push_back(*text);
      return;
    }
  }

  if (!value.is_none()) {
    out->push_back(bridge_support::SerializeJson(value.Clone()));
  }
}

std::string FormatTextValue(const Value* value) {
  if (!value) {
    return std::string();
  }

  std::vector<std::string> parts;
  AppendFormattedText(*value, &parts);

  std::string result;
  for (const std::string& part : parts) {
    if (part.empty()) {
      continue;
    }
    if (!result.empty()) {
      result.append("\n");
    }
    result.append(part);
  }
  return result;
}

std::string GenerateRandomId() {
  return base::Uuid::GenerateRandomV4().AsLowercaseString();
}

NexusLlmBridgePendingToolCall MakePendingToolCall(
    std::string tool_use_id,
    std::optional<std::string> name) {
  NexusLlmBridgePendingToolCall pending;
  pending.tool_use_id = std::move(tool_use_id);
  pending.name = std::move(name);
  pending.args_text = std::string();
  return pending;
}

Dict UsageToBridge(const mojom::NexusLlmUsage* usage) {
  Dict bridge_usage;
  bridge_usage.Set("input_tokens", usage ? usage->prompt_tokens : 0);
  bridge_usage.Set("output_tokens", usage ? usage->completion_tokens : 0);
  bridge_usage.Set("cache_read_input_tokens",
                   usage ? usage->cache_read_input_tokens : 0);
  bridge_usage.Set("cache_creation_input_tokens",
                   usage ? usage->cache_creation_input_tokens : 0);
  return bridge_usage;
}

std::optional<mojom::NexusLlmRole> ToMojoRole(std::string_view role) {
  if (role == "system") {
    return mojom::NexusLlmRole::kSystem;
  }
  if (role == "user") {
    return mojom::NexusLlmRole::kUser;
  }
  if (role == "assistant") {
    return mojom::NexusLlmRole::kAssistant;
  }
  if (role == "tool") {
    return mojom::NexusLlmRole::kTool;
  }
  return std::nullopt;
}

std::string FinishReasonToStopReason(mojom::NexusLlmFinishReason reason) {
  switch (reason) {
    case mojom::NexusLlmFinishReason::kLength:
      return "max_tokens";
    case mojom::NexusLlmFinishReason::kToolCalls:
      return "tool_use";
    case mojom::NexusLlmFinishReason::kContentFilter:
      return "content_filter";
    case mojom::NexusLlmFinishReason::kError:
      return "error";
    case mojom::NexusLlmFinishReason::kStop:
    case mojom::NexusLlmFinishReason::kOther:
    case mojom::NexusLlmFinishReason::kUnknown:
      return "end_turn";
  }
  return "end_turn";
}

mojom::NexusLlmContentPartPtr MakeTextContentPart(const std::string& text) {
  auto part = mojom::NexusLlmContentPart::New();
  part->type = mojom::NexusLlmContentPartType::kText;
  part->text = text;
  return part;
}

bool FailInvalidBridgeFormat(std::string* error_message,
                             const std::string& message) {
  // Bridge payloads carry model-generated and persisted-session content from
  // the guest, so a malformed part — e.g. a tool-call with an empty toolCallId
  // left behind by a streaming-parse bug, or a session saved by an older
  // version — is a NORMAL error case, not an internal invariant. Reject it with
  // the graceful error below (propagated to the guest as an INVALID_ARGS reply)
  // instead of a DCHECK, which would crash the browser on untrusted input.
  DLOG(ERROR) << "Invalid bridge format: " << message;
  if (error_message) {
    *error_message = message;
  }
  return false;
}

// Like FailInvalidBridgeFormat, but for genuine protocol-DIALECT violations:
// the guest speaking a LEGACY bridge format (id/name/input parts,
// tool_use/tool_result content types, legacy message wrappers) instead of the
// canonical one. That only happens when the guest is out of sync with the
// browser — an invariant worth a loud DCHECK in debug (and asserted by
// BuildMojoLlmRequestRejectsLegacyToolFormats). FailInvalidBridgeFormat above
// instead covers MALFORMED data (empty/missing fields, bad shapes) that a
// persisted session or a prior streaming-parse bug can legitimately produce;
// that must be rejected gracefully, never crash the browser.
bool DcheckInvalidBridgeFormat(std::string* error_message,
                               const std::string& message) {
  DCHECK(false) << message;
  if (error_message) {
    *error_message = message;
  }
  return false;
}

bool HasField(const Dict& dict, const char* key) {
  return dict.Find(key) != nullptr;
}

bool ValidateCanonicalToolCallPart(const Dict& part_dict,
                                   std::string* error_message) {
  if (HasField(part_dict, "id") || HasField(part_dict, "name") ||
      HasField(part_dict, "input")) {
    return DcheckInvalidBridgeFormat(
        error_message,
        "Bridge tool-call parts must use toolCallId/toolName/args, not "
        "legacy id/name/input fields");
  }

  if (!bridge_support::FindNonEmptyString(part_dict, "toolCallId")) {
    return FailInvalidBridgeFormat(
        error_message, "Bridge tool-call parts must include toolCallId");
  }
  if (!bridge_support::FindNonEmptyString(part_dict, "toolName")) {
    return FailInvalidBridgeFormat(
        error_message, "Bridge tool-call parts must include toolName");
  }

  return true;
}

bool ValidateCanonicalToolResultPart(const Dict& part_dict,
                                     std::string* error_message) {
  if (HasField(part_dict, "tool_use_id") || HasField(part_dict, "tool_name") ||
      HasField(part_dict, "name") || HasField(part_dict, "content") ||
      HasField(part_dict, "is_error")) {
    return DcheckInvalidBridgeFormat(
        error_message,
        "Bridge tool-result parts must use toolCallId/toolName/result/isError,"
        " not legacy tool_use_id/tool_name/content/is_error fields");
  }

  if (!bridge_support::FindNonEmptyString(part_dict, "toolCallId")) {
    return FailInvalidBridgeFormat(
        error_message, "Bridge tool-result parts must include toolCallId");
  }
  if (!HasField(part_dict, "result")) {
    return FailInvalidBridgeFormat(
        error_message, "Bridge tool-result parts must include result");
  }

  return true;
}

bool ValidateCanonicalBridgeContent(const Value& part_value,
                                    std::string* error_message) {
  if (part_value.is_string()) {
    return true;
  }

  const Dict* part_dict = part_value.GetIfDict();
  if (!part_dict) {
    return FailInvalidBridgeFormat(
        error_message,
        "Bridge content parts must be strings or typed objects");
  }

  const std::string* type = part_dict->FindString("type");
  if (!type || type->empty()) {
    return FailInvalidBridgeFormat(
        error_message, "Bridge content part objects must include type");
  }

  if (*type == "tool_use" || *type == "tool_result") {
    return DcheckInvalidBridgeFormat(
        error_message,
        "Bridge content parts must use canonical tool-call/tool-result types");
  }

  if (*type == "tool-call") {
    return ValidateCanonicalToolCallPart(*part_dict, error_message);
  }
  if (*type == "tool-result") {
    return ValidateCanonicalToolResultPart(*part_dict, error_message);
  }

  if (*type == "text" || *type == "thinking" || *type == "redacted_thinking" ||
      *type == "image") {
    return true;
  }

  return FailInvalidBridgeFormat(
      error_message,
      "Bridge content part type \"" + *type + "\" is not supported");
}

bool ValidateCanonicalBridgeMessages(const Value* messages_value,
                                     std::string* error_message) {
  if (!messages_value || !messages_value->is_list()) {
    *error_message = "LLM request \"messages\" must be an array";
    return false;
  }

  for (const Value& entry_value : messages_value->GetList()) {
    const Dict* entry = entry_value.GetIfDict();
    if (!entry) {
      return FailInvalidBridgeFormat(
          error_message, "Bridge messages must be objects");
    }
    if (entry->Find("message") != nullptr || entry->Find("type") != nullptr) {
      return DcheckInvalidBridgeFormat(
          error_message,
          "Bridge messages must use plain {role, content} payloads without "
          "legacy wrapper fields");
    }

    std::optional<std::string> role_string =
        bridge_support::FindNonEmptyString(*entry, "role");
    if (!role_string || !ToMojoRole(*role_string).has_value()) {
      return FailInvalidBridgeFormat(
          error_message,
          "Bridge messages must include a valid role");
    }

    const Value* content = entry->Find("content");
    if (!content) {
      return FailInvalidBridgeFormat(
          error_message, "Bridge messages must include content");
    }
    if (content->is_list()) {
      for (const Value& part : content->GetList()) {
        if (!ValidateCanonicalBridgeContent(part, error_message)) {
          return false;
        }
      }
    } else if (!content->is_string()) {
      return FailInvalidBridgeFormat(
          error_message,
          "Bridge message content must be a string or an array of typed parts");
    }
  }

  return true;
}

bool ValidateCanonicalBridgeTools(const Value* tools_value,
                                  std::string* error_message) {
  if (!tools_value) {
    return true;
  }
  if (!tools_value->is_list()) {
    *error_message = "LLM request \"tools\" must be an array";
    return false;
  }

  for (const Value& entry_value : tools_value->GetList()) {
    const Dict* entry = entry_value.GetIfDict();
    if (!entry) {
      return FailInvalidBridgeFormat(
          error_message, "Bridge tool definitions must be objects");
    }

    // A `{ ref: name }` entry is resolved to a full definition from the
    // C++ ToolCatalog in ConvertBridgeTools; it is valid here without the
    // full-def fields below.
    if (bridge_support::FindNonEmptyString(*entry, "ref")) {
      continue;
    }

    const std::string* type = entry->FindString("type");
    if (!type || *type != "function") {
      return FailInvalidBridgeFormat(
          error_message,
          "Bridge tool definitions must use type=\"function\"");
    }
    if (entry->Find("input_schema") != nullptr) {
      return FailInvalidBridgeFormat(
          error_message,
          "Bridge tool definitions must use parameters, not input_schema");
    }
    if (!bridge_support::FindNonEmptyString(*entry, "name")) {
      return FailInvalidBridgeFormat(
          error_message, "Bridge tool definitions must include name");
    }
    if (!entry->FindDict("parameters")) {
      return FailInvalidBridgeFormat(
          error_message,
          "Bridge tool definitions must include a parameters object");
    }
  }

  return true;
}

void AppendBridgeContentPart(const Value& part_value,
                             std::vector<mojom::NexusLlmContentPartPtr>* out) {
  if (const std::string* text = part_value.GetIfString()) {
    out->push_back(MakeTextContentPart(*text));
    return;
  }

  const Dict* part_dict = part_value.GetIfDict();
  if (!part_dict) {
    return;
  }

  const std::string* type = part_dict->FindString("type");
  if (!type) {
    return;
  }

  if (*type == "text") {
    out->push_back(
        MakeTextContentPart(FormatTextValue(part_dict->Find("text"))));
    return;
  }

  if (*type == "thinking") {
    auto part = mojom::NexusLlmContentPart::New();
    part->type = mojom::NexusLlmContentPartType::kThinking;
    part->text = FormatTextValue(part_dict->Find("thinking"));
    if (std::optional<std::string> signature =
            bridge_support::FindNonEmptyString(*part_dict, "signature")) {
      part->signature = *signature;
    }
    out->push_back(std::move(part));
    return;
  }

  if (*type == "redacted_thinking") {
    auto part = mojom::NexusLlmContentPart::New();
    part->type = mojom::NexusLlmContentPartType::kRedactedThinking;
    out->push_back(std::move(part));
    return;
  }

  if (*type == "image") {
    std::optional<std::string> image =
        bridge_support::FindNonEmptyString(*part_dict, "image");
    if (!image) {
      image = bridge_support::FindNonEmptyString(*part_dict, "data");
    }
    if (!image) {
      image = bridge_support::FindNonEmptyString(*part_dict, "url");
    }
    if (!image) {
      return;
    }

    auto part = mojom::NexusLlmContentPart::New();
    part->type = mojom::NexusLlmContentPartType::kImage;
    part->image = *image;
    std::optional<std::string> mime_type =
        bridge_support::FindNonEmptyString(*part_dict, "mimeType");
    if (mime_type) {
      part->mime_type = *mime_type;
    } else {
      mime_type =
          bridge_support::FindNonEmptyString(*part_dict, "mime_type");
    }
    if (mime_type) {
      part->mime_type = *mime_type;
    }
    out->push_back(std::move(part));
    return;
  }

  if (*type == "tool-call") {
    std::optional<std::string> tool_use_id =
        bridge_support::FindNonEmptyString(*part_dict, "toolCallId");
    std::optional<std::string> name =
        bridge_support::FindNonEmptyString(*part_dict, "toolName");
    if (!tool_use_id || !name) {
      return;
    }

    auto part = mojom::NexusLlmContentPart::New();
    part->type = mojom::NexusLlmContentPartType::kToolUse;
    part->tool_use_id = *tool_use_id;
    part->name = *name;
    if (const Value* args = part_dict->Find("args")) {
      part->input = args->Clone();
    } else {
      part->input = Value(Dict());
    }
    out->push_back(std::move(part));
    return;
  }

  if (*type == "tool-result") {
    auto part = mojom::NexusLlmContentPart::New();
    part->type = mojom::NexusLlmContentPartType::kToolResult;

    std::optional<std::string> tool_use_id =
        bridge_support::FindNonEmptyString(*part_dict, "toolCallId");
    if (tool_use_id) {
      part->tool_use_id = *tool_use_id;
    }

    std::optional<std::string> name =
        bridge_support::FindNonEmptyString(*part_dict, "toolName");
    if (name) {
      part->name = *name;
    }

    if (const Value* result = part_dict->Find("result")) {
      part->result = result->Clone();
    } else {
      part->result = Value(std::string());
    }

    std::optional<bool> is_error = part_dict->FindBool("isError");
    if (is_error) {
      part->is_error = *is_error;
    }

    out->push_back(std::move(part));
  }
}

std::vector<mojom::NexusLlmContentPartPtr> ConvertBridgeContent(
    const Value* content_value) {
  std::vector<mojom::NexusLlmContentPartPtr> converted;
  if (!content_value) {
    return converted;
  }

  if (const std::string* text = content_value->GetIfString()) {
    converted.push_back(MakeTextContentPart(*text));
    return converted;
  }

  if (!content_value->is_list()) {
    const std::string text = FormatTextValue(content_value);
    if (!text.empty()) {
      converted.push_back(MakeTextContentPart(text));
    }
    return converted;
  }

  for (const Value& part : content_value->GetList()) {
    AppendBridgeContentPart(part, &converted);
  }
  return converted;
}

bool ConvertBridgeMessages(const Value* messages_value,
                           std::vector<mojom::NexusLlmMessagePtr>* out,
                           std::string* error_message) {
  if (!ValidateCanonicalBridgeMessages(messages_value, error_message)) {
    return false;
  }

  for (const Value& entry_value : messages_value->GetList()) {
    const Dict* entry = entry_value.GetIfDict();
    if (!entry) {
      continue;
    }

    std::optional<std::string> role_string =
        bridge_support::FindNonEmptyString(*entry, "role");
    std::optional<mojom::NexusLlmRole> role =
        role_string ? ToMojoRole(*role_string) : std::nullopt;
    if (!role) {
      continue;
    }

    std::vector<mojom::NexusLlmContentPartPtr> content =
        ConvertBridgeContent(entry->Find("content"));
    if (content.empty()) {
      continue;
    }

    auto message = mojom::NexusLlmMessage::New();
    message->role = *role;
    message->content = std::move(content);
    out->push_back(std::move(message));
  }

  return true;
}

bool ConvertBridgeTools(const Value* tools_value,
                        bool strict_tools,
                        std::vector<mojom::NexusLlmToolPtr>* out,
                        std::string* error_message) {
  if (!ValidateCanonicalBridgeTools(tools_value, error_message)) {
    return false;
  }

  if (!tools_value) {
    return true;
  }
  for (const Value& entry_value : tools_value->GetList()) {
    const Dict* entry = entry_value.GetIfDict();
    if (!entry) {
      continue;
    }

    // Resolve a `{ ref: name }` to the catalog's full definition dict;
    // otherwise the entry is already a full definition. An unknown ref is
    // skipped (matches the JS resolver contract).
    Dict resolved_storage;
    const Dict* def = entry;
    bool was_ref = false;
    if (std::optional<std::string> ref =
            bridge_support::FindNonEmptyString(*entry, "ref");
        ref && !entry->FindDict("parameters")) {
      // Resolve from the C++ ToolCatalog (host tools) first, then the
      // runtime registry (static-JS / MCP defs published via
      // model.registerToolDefs). Unknown ref → skip.
      if (std::optional<Dict> from_catalog = ResolveToolRefToDict(*ref)) {
        resolved_storage = std::move(*from_catalog);
      } else if (const Dict* registered =
                     PromptHolderRegistry::Get().ResolveToolDef(*ref)) {
        resolved_storage = registered->Clone();
      } else {
        continue;
      }
      def = &resolved_storage;
      was_ref = true;
    }

    std::optional<std::string> name =
        bridge_support::FindNonEmptyString(*def, "name");
    if (!name) {
      continue;
    }

    auto tool = mojom::NexusLlmTool::New();
    tool->name = *name;
    if (std::optional<std::string> description =
            bridge_support::FindNonEmptyString(*def, "description")) {
      tool->description = *description;
    }

    // Schema selection: a resolved ref under a strict-eligible model uses the
    // tool's strict `parametersStrict` variant (and sets strict=true);
    // otherwise the regular `parameters`. Full (non-ref) defs already carry
    // their chosen schema + strict flag from the JS side.
    const Dict* schema_source = def->FindDict("parameters");
    if (was_ref && strict_tools) {
      if (const Dict* strict_schema = def->FindDict("parametersStrict")) {
        schema_source = strict_schema;
        tool->strict = true;
      }
    }
    if (schema_source) {
      tool->input_schema = schema_source->Clone();
    } else {
      Dict default_schema;
      default_schema.Set("type", "object");
      tool->input_schema = std::move(default_schema);
    }

    // Forward an explicit `strict` flag when the definition carries one:
    // non-ref full defs and registry-resolved JS/MCP refs both do. Catalog
    // (`BuildHostedToolDefinitionDict`) refs carry none — their strict flag
    // was decided above from `strict_tools` + `parametersStrict`.
    if (auto strict = def->FindBool("strict")) {
      tool->strict = *strict;
    }

    // Forward optional canonical example calls. The Anthropic provider
    // translator surfaces them as `input_examples` on the wire; other
    // providers silently drop. Each entry must validate against
    // `input_schema` — Anthropic returns 400 on any violation.
    if (const List* examples = def->FindList("inputExamples")) {
      tool->input_examples = examples->Clone();
    }

    out->push_back(std::move(tool));
  }

  return true;
}

std::optional<Dict> FromMojoContentPart(const mojom::NexusLlmContentPart& part) {
  Dict bridge_part;
  switch (part.type) {
    case mojom::NexusLlmContentPartType::kText:
      bridge_part.Set("type", "text");
      bridge_part.Set("text", part.text.value_or(std::string()));
      return bridge_part;

    case mojom::NexusLlmContentPartType::kThinking:
      bridge_part.Set("type", "thinking");
      bridge_part.Set("thinking", part.text.value_or(std::string()));
      if (part.signature.has_value() && !part.signature->empty()) {
        bridge_part.Set("signature", *part.signature);
      }
      return bridge_part;

    case mojom::NexusLlmContentPartType::kRedactedThinking:
      bridge_part.Set("type", "redacted_thinking");
      return bridge_part;

    case mojom::NexusLlmContentPartType::kImage:
      bridge_part.Set("type", "image");
      if (part.image.has_value()) {
        bridge_part.Set("image", *part.image);
      }
      if (part.mime_type.has_value() && !part.mime_type->empty()) {
        bridge_part.Set("mimeType", *part.mime_type);
      }
      return bridge_part;

    case mojom::NexusLlmContentPartType::kToolUse:
      bridge_part.Set("type", "tool-call");
      bridge_part.Set("toolCallId", part.tool_use_id.value_or(std::string()));
      bridge_part.Set("toolName", part.name.value_or(std::string()));
      if (part.input.has_value()) {
        bridge_part.Set("args", part.input->Clone());
      } else {
        bridge_part.Set("args", Value(Dict()));
      }
      return bridge_part;

    case mojom::NexusLlmContentPartType::kToolResult:
      bridge_part.Set("type", "tool-result");
      if (part.result.has_value()) {
        bridge_part.Set("result", part.result->Clone());
      } else {
        bridge_part.Set("result", Value(std::string()));
      }
      if (part.tool_use_id.has_value() && !part.tool_use_id->empty()) {
        bridge_part.Set("toolCallId", *part.tool_use_id);
      }
      if (part.name.has_value() && !part.name->empty()) {
        bridge_part.Set("toolName", *part.name);
      }
      if (part.is_error.has_value()) {
        bridge_part.Set("isError", *part.is_error);
      }
      return bridge_part;
  }
  return std::nullopt;
}

Value ParseJsonStringOrReturnString(const std::string& value) {
  std::optional<Value> parsed =
      base::JSONReader::Read(value, base::JSON_PARSE_RFC);
  if (parsed.has_value()) {
    return std::move(*parsed);
  }
  return Value(value);
}

void CloseOpenDeltaBlock(NexusLlmBridgeStreamState* state, List* chunks) {
  if (!state->open_delta_block.has_value()) {
    return;
  }

  Dict chunk;
  chunk.Set("type", "content_block_stop");
  chunk.Set("index", state->open_delta_block->index);
  chunks->Append(std::move(chunk));
  state->open_delta_block.reset();
}

int EnsureDeltaBlockOpen(NexusLlmBridgeStreamState* state,
                         List* chunks,
                         NexusLlmBridgeDeltaBlockType type) {
  if (state->open_delta_block.has_value() &&
      state->open_delta_block->type == type) {
    return state->open_delta_block->index;
  }

  CloseOpenDeltaBlock(state, chunks);

  const int index = state->next_content_block_index++;

  Dict content_block;
  if (type == NexusLlmBridgeDeltaBlockType::kThinking) {
    content_block.Set("type", "thinking");
    content_block.Set("thinking", "");
  } else {
    content_block.Set("type", "text");
    content_block.Set("text", "");
  }

  Dict chunk;
  chunk.Set("type", "content_block_start");
  chunk.Set("index", index);
  chunk.Set("content_block", std::move(content_block));
  chunks->Append(std::move(chunk));

  state->open_delta_block = NexusLlmBridgeOpenDeltaBlock{type, index};
  return index;
}

void EmitToolCallChunk(NexusLlmBridgeStreamState* state,
                       List* chunks,
                       const std::string& tool_use_id,
                       const std::optional<std::string>& name,
                       Value input) {
  CloseOpenDeltaBlock(state, chunks);

  const int index = state->next_content_block_index++;

  Dict content_block;
  content_block.Set("type", "tool-call");
  content_block.Set("toolCallId", tool_use_id);
  content_block.Set("toolName", name.value_or("tool"));
  content_block.Set("args", std::move(input));

  Dict start_chunk;
  start_chunk.Set("type", "content_block_start");
  start_chunk.Set("index", index);
  start_chunk.Set("content_block", std::move(content_block));
  chunks->Append(std::move(start_chunk));

  Dict stop_chunk;
  stop_chunk.Set("type", "content_block_stop");
  stop_chunk.Set("index", index);
  chunks->Append(std::move(stop_chunk));
}

void FlushPendingToolCalls(NexusLlmBridgeStreamState* state, List* chunks) {
  for (const std::string& tool_use_id : state->pending_tool_call_order) {
    auto pending_it = state->pending_tool_calls.find(tool_use_id);
    if (pending_it == state->pending_tool_calls.end()) {
      continue;
    }

    EmitToolCallChunk(
        state, chunks, pending_it->second.tool_use_id, pending_it->second.name,
        pending_it->second.args_text.empty()
            ? Value(Dict())
            : ParseJsonStringOrReturnString(pending_it->second.args_text));
  }

  state->pending_tool_calls.clear();
  state->pending_tool_call_order.clear();
}

void FinalizeStream(NexusLlmBridgeStreamState* state,
                    List* chunks,
                    std::optional<mojom::NexusLlmFinishReason> finish_reason,
                    const mojom::NexusLlmUsage* usage) {
  CloseOpenDeltaBlock(state, chunks);
  FlushPendingToolCalls(state, chunks);

  if (finish_reason.has_value() || usage) {
    Dict delta;
    if (finish_reason.has_value()) {
      delta.Set("stop_reason", FinishReasonToStopReason(*finish_reason));
    }

    Dict chunk;
    chunk.Set("type", "message_delta");
    if (!delta.empty()) {
      chunk.Set("delta", std::move(delta));
    }
    if (usage) {
      chunk.Set("usage", UsageToBridge(usage));
    }
    chunks->Append(std::move(chunk));
  }

  Dict stop_chunk;
  stop_chunk.Set("type", "message_stop");
  chunks->Append(std::move(stop_chunk));
  state->terminal_emitted = true;
}

}  // namespace

NexusLlmBridgePendingToolCall::NexusLlmBridgePendingToolCall() = default;

NexusLlmBridgePendingToolCall::NexusLlmBridgePendingToolCall(
    const NexusLlmBridgePendingToolCall&) = default;

NexusLlmBridgePendingToolCall::NexusLlmBridgePendingToolCall(
    NexusLlmBridgePendingToolCall&&) = default;

NexusLlmBridgePendingToolCall& NexusLlmBridgePendingToolCall::operator=(
    const NexusLlmBridgePendingToolCall&) = default;

NexusLlmBridgePendingToolCall& NexusLlmBridgePendingToolCall::operator=(
    NexusLlmBridgePendingToolCall&&) = default;

NexusLlmBridgePendingToolCall::~NexusLlmBridgePendingToolCall() = default;

NexusLlmBridgeStreamState::NexusLlmBridgeStreamState() = default;

NexusLlmBridgeStreamState::NexusLlmBridgeStreamState(
    const NexusLlmBridgeStreamState&) = default;

NexusLlmBridgeStreamState::NexusLlmBridgeStreamState(
    NexusLlmBridgeStreamState&&) = default;

NexusLlmBridgeStreamState& NexusLlmBridgeStreamState::operator=(
    const NexusLlmBridgeStreamState&) = default;

NexusLlmBridgeStreamState& NexusLlmBridgeStreamState::operator=(
    NexusLlmBridgeStreamState&&) = default;

NexusLlmBridgeStreamState::~NexusLlmBridgeStreamState() = default;

mojom::NexusLlmRequestParamsPtr BuildMojoLlmRequest(
    const Dict& payload,
    std::string* error_message) {
  auto request = mojom::NexusLlmRequestParams::New();

  if (!ConvertBridgeMessages(payload.Find("messages"), &request->messages,
                             error_message)) {
    return nullptr;
  }
  const bool strict_tools = payload.FindBool("strictTools").value_or(false);
  if (!ConvertBridgeTools(payload.Find("tools"), strict_tools, &request->tools,
                          error_message)) {
    return nullptr;
  }

  if (const Value* system_prompt = payload.Find("systemPrompt")) {
    request->system_prompt = system_prompt->Clone();
  }
  if (const Dict* user_context = payload.FindDict("userContext")) {
    request->user_context = user_context->Clone();
  }
  if (const Dict* system_context = payload.FindDict("systemContext")) {
    request->system_context = system_context->Clone();
  }
  if (const Dict* tool_use_context = payload.FindDict("toolUseContext")) {
    request->tool_use_context = tool_use_context->Clone();
  }
  if (std::optional<std::string> provider_id =
          bridge_support::FindNonEmptyString(payload, "providerId")) {
    request->provider_id = *provider_id;
  }
  if (std::optional<std::string> model_id =
          bridge_support::FindNonEmptyString(payload, "model")) {
    request->model_id = *model_id;
  }
  if (std::optional<int64_t> max_output_tokens =
          bridge_support::FindIntegralValue(payload, "maxOutputTokens")) {
    if (!base::IsValueInRangeForNumericType<int32_t>(*max_output_tokens)) {
      *error_message = "\"maxOutputTokens\" is out of range";
      return nullptr;
    }
    request->max_output_tokens =
        base::checked_cast<int32_t>(*max_output_tokens);
  }
  if (const Dict* ext = payload.FindDict("providerBodyExtension")) {
    request->provider_body_extension = ext->Clone();
  }

  return request;
}

base::DictValue CatalogToBridge(const mojom::NexusLlmCatalog& catalog) {
  Dict result;
  List providers;

  for (const auto& provider : catalog.providers) {
    if (!provider) {
      continue;
    }

    Dict provider_dict;
    provider_dict.Set("id", provider->id);
    provider_dict.Set("label", provider->label);
    provider_dict.Set("iconSvg", provider->icon_svg);
    provider_dict.Set("enabled", provider->enabled);
    if (provider->default_model_id.has_value() &&
        !provider->default_model_id->empty()) {
      provider_dict.Set("defaultModelId", *provider->default_model_id);
    }

    List models;
    for (const auto& model : provider->models) {
      if (!model) {
        continue;
      }

      Dict model_dict;
      model_dict.Set("id", model->id);
      model_dict.Set("label", model->label);
      model_dict.Set("enabled", model->enabled);
      model_dict.Set("supportsStreaming", model->supports_streaming);
      model_dict.Set("supportsTools", model->supports_tools);
      if (model->default_thinking_tier.has_value() &&
          !model->default_thinking_tier->empty()) {
        model_dict.Set("defaultThinkingTier", *model->default_thinking_tier);
      }
      if (!model->available_thinking_tiers.empty()) {
        List tiers;
        for (const auto& tier : model->available_thinking_tiers) {
          tiers.Append(tier);
        }
        model_dict.Set("availableThinkingTiers", std::move(tiers));
      }
      models.Append(std::move(model_dict));
    }

    provider_dict.Set("models", std::move(models));
    providers.Append(std::move(provider_dict));
  }

  result.Set("providers", std::move(providers));

  if (catalog.default_selection) {
    Dict selection;
    selection.Set("providerId", catalog.default_selection->provider_id);
    selection.Set("modelId", catalog.default_selection->model_id);
    result.Set("defaultSelection", std::move(selection));
  }

  if (!catalog.raw_catalogs.empty()) {
    Dict raw_catalogs;
    for (const auto& [provider_id, raw] : catalog.raw_catalogs) {
      raw_catalogs.Set(provider_id, raw);
    }
    result.Set("rawCatalogs", std::move(raw_catalogs));
  }

  return result;
}

base::DictValue CallResultToBridge(const mojom::NexusLlmCallResult& result) {
  Dict message_dict;
  message_dict.Set("role", "assistant");

  List content;
  if (result.message) {
    for (const auto& part : result.message->content) {
      if (!part) {
        continue;
      }
      if (std::optional<Dict> bridge_part = FromMojoContentPart(*part)) {
        content.Append(std::move(*bridge_part));
      }
    }
  }
  message_dict.Set("content", std::move(content));

  if (result.message && result.message->model_id.has_value() &&
      !result.message->model_id->empty()) {
    message_dict.Set("model", *result.message->model_id);
  }

  message_dict.Set("stop_reason",
                   FinishReasonToStopReason(
                       result.message ? result.message->finish_reason
                                      : mojom::NexusLlmFinishReason::kUnknown));
  message_dict.Set(
      "usage",
      UsageToBridge(result.message ? result.message->usage.get() : nullptr));

  Dict response;
  response.Set("message", std::move(message_dict));
  // Surface the host-side outcome + timeline when present (native deadline /
  // hold-read contract). Absent on older hosts — the JS guest treats missing
  // status as 'ok'.
  if (result.status.has_value() && !result.status->empty()) {
    response.Set("status", *result.status);
  }
  if (result.timeline.has_value() && !result.timeline->empty()) {
    response.Set("timeline", result.timeline->Clone());
  }
  return response;
}

base::DictValue StreamReadResultToBridge(
    NexusLlmBridgeStreamState* state,
    const mojom::NexusLlmStreamReadResult& result,
    bool* done_out,
    bool* should_cancel_out) {
  List chunks;

  for (const auto& event : result.events) {
    if (!event) {
      continue;
    }

    switch (event->type) {
      case mojom::NexusLlmStreamEventType::kMessageStart: {
        Dict chunk;
        chunk.Set("type", "message_start");
        Dict message;
        if (event->model_id.has_value() && !event->model_id->empty()) {
          message.Set("model", *event->model_id);
        }
        if (event->usage) {
          message.Set("usage", UsageToBridge(event->usage.get()));
        }
        chunk.Set("message", std::move(message));
        chunks.Append(std::move(chunk));
        break;
      }

      case mojom::NexusLlmStreamEventType::kTextDelta: {
        const int index = EnsureDeltaBlockOpen(
            state, &chunks, NexusLlmBridgeDeltaBlockType::kText);

        Dict delta;
        delta.Set("type", "text_delta");
        delta.Set("text", event->text_delta.value_or(std::string()));

        Dict chunk;
        chunk.Set("type", "content_block_delta");
        chunk.Set("index", index);
        chunk.Set("delta", std::move(delta));
        chunks.Append(std::move(chunk));
        break;
      }

      case mojom::NexusLlmStreamEventType::kReasoningDelta: {
        const int index = EnsureDeltaBlockOpen(
            state, &chunks, NexusLlmBridgeDeltaBlockType::kThinking);

        Dict delta;
        delta.Set("type", "thinking_delta");
        delta.Set("thinking", event->text_delta.value_or(std::string()));

        Dict chunk;
        chunk.Set("type", "content_block_delta");
        chunk.Set("index", index);
        chunk.Set("delta", std::move(delta));
        chunks.Append(std::move(chunk));
        break;
      }

      case mojom::NexusLlmStreamEventType::kToolCallStart: {
        const std::string tool_use_id =
            event->tool_use_id.value_or(GenerateRandomId());
        if (state->pending_tool_calls.find(tool_use_id) ==
            state->pending_tool_calls.end()) {
          state->pending_tool_call_order.push_back(tool_use_id);
          state->pending_tool_calls.emplace(
              tool_use_id, MakePendingToolCall(tool_use_id, event->name));
        }
        break;
      }

      case mojom::NexusLlmStreamEventType::kToolCallDelta: {
        const std::string tool_use_id =
            event->tool_use_id.value_or(GenerateRandomId());
        auto [pending_it, inserted] = state->pending_tool_calls.emplace(
            tool_use_id, MakePendingToolCall(tool_use_id, event->name));
        if (inserted) {
          state->pending_tool_call_order.push_back(tool_use_id);
        }

        if (event->name.has_value() && !event->name->empty()) {
          pending_it->second.name = event->name;
        }
        if (event->args_text_delta.has_value() &&
            !event->args_text_delta->empty()) {
          pending_it->second.args_text.append(*event->args_text_delta);
        }
        break;
      }

      case mojom::NexusLlmStreamEventType::kToolCall: {
        const std::string tool_use_id =
            event->tool_use_id.value_or(GenerateRandomId());

        std::optional<NexusLlmBridgePendingToolCall> pending;
        auto pending_it = state->pending_tool_calls.find(tool_use_id);
        if (pending_it != state->pending_tool_calls.end()) {
          pending = std::move(pending_it->second);
          state->pending_tool_calls.erase(pending_it);
        }
        state->pending_tool_call_order.erase(
            std::remove(state->pending_tool_call_order.begin(),
                        state->pending_tool_call_order.end(), tool_use_id),
            state->pending_tool_call_order.end());

        std::optional<std::string> tool_name =
            event->name.has_value() && !event->name->empty() ? event->name
            : pending                                        ? pending->name
                                                             : std::nullopt;
        Value input = event->input.has_value() ? event->input->Clone()
                      : pending && !pending->args_text.empty()
                          ? ParseJsonStringOrReturnString(pending->args_text)
                          : Value(Dict());

        EmitToolCallChunk(state, &chunks, tool_use_id, tool_name,
                          std::move(input));
        break;
      }

      case mojom::NexusLlmStreamEventType::kFinish:
        FinalizeStream(state, &chunks, event->finish_reason,
                       event->usage.get());
        break;

      case mojom::NexusLlmStreamEventType::kError: {
        CloseOpenDeltaBlock(state, &chunks);

        Dict error;
        error.Set("type", "provider_error");
        error.Set("message", event->error_message.value_or(
                                 std::string("Unknown LLM stream error")));

        Dict chunk;
        chunk.Set("type", "error");
        chunk.Set("error", std::move(error));
        chunks.Append(std::move(chunk));
        state->terminal_emitted = true;
        break;
      }
    }
  }

  bool done = result.done || state->terminal_emitted;
  if (done && !state->terminal_emitted) {
    FinalizeStream(state, &chunks, std::nullopt, nullptr);
  }

  *done_out = done;
  *should_cancel_out = done && !result.done;

  Dict response;
  response.Set("chunks", std::move(chunks));
  response.Set("done", done);
  // Surface host-side outcome + timeline (hold-read contract). On a native
  // timeout the host has already cancelled the upstream loader; the JS guest
  // just logs the timeline and stops polling.
  if (result.status.has_value() && !result.status->empty()) {
    response.Set("status", *result.status);
  }
  if (result.timeline.has_value() && !result.timeline->empty()) {
    response.Set("timeline", result.timeline->Clone());
  }
  return response;
}

}  // namespace nexus
