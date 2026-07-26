#include "custom_browser/browser/nexus/api/llm/nexus_llm_host.h"

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include "base/barrier_closure.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/task/sequenced_task_runner.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/uuid.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/grit/custom_browser_resources.h"
#include "components/prefs/pref_service.h"
#include "custom_browser/browser/nexus/api/catalog/prompt_holder_registry.h"
#include "custom_browser/browser/nexus/api/catalog/prompt_section_renderer.h"
#include "custom_browser/browser/nexus/api/llm/llm_broker.h"
#include "custom_browser/browser/nexus/api/llm/llm_broker_factory.h"
#include "custom_browser/browser/nexus/api/llm/llm_pref_names.h"
#include "custom_browser/browser/nexus/api/llm/llm_provider.h"
#include "custom_browser/browser/nexus/api/llm/llm_text_helpers.h"
#include "custom_browser/browser/nexus/api/llm/oauth/oauth_manager.h"
#include "custom_browser/browser/nexus/api/llm/oauth/oauth_providers.h"
#include "custom_browser/browser/nexus/api/llm/provider_config.h"
#include "mojo/public/cpp/base/values_mojom_traits.h"
#include "ui/base/resource/resource_bundle.h"

namespace nexus {

namespace {

using ToolNameMap = std::map<std::string, std::string>;

constexpr char kToolChoiceAuto[] = "auto";
constexpr base::TimeDelta kCatalogRefreshInterval = base::Seconds(30);

bool IsModelEnabled(PrefService* prefs,
                    const std::string& provider_id,
                    const std::string& model_id) {
  const base::DictValue* config = GetStoredProviderConfig(prefs, provider_id);
  if (!config) {
    return true;
  }
  const base::ListValue* enabled_models = config->FindList("enabled_models");
  if (!enabled_models || enabled_models->empty()) {
    return true;
  }
  for (const auto& value : *enabled_models) {
    const std::string* enabled_model = value.GetIfString();
    if (enabled_model && *enabled_model == model_id) {
      return true;
    }
  }
  return false;
}

// Mirror of `deriveModelId` in nexus_quick_settings.ts. The TS curated
// catalog stores each model's `model` field as either a plain id or a
// regex source (anchored, optionally suffixed with a date pattern that
// the OpenAI/Anthropic helpers add). The worker's `isSelectionEnabled`
// gate compares against the literal id the picker sends — so we strip
// the anchors / common date suffixes, then bail if the result still has
// regex metacharacters we don't know how to flatten.
std::string DeriveTsModelId(const std::string& pattern) {
  if (pattern.empty()) return std::string();
  std::string s = pattern;
  if (!s.empty() && s.front() == '^') s.erase(s.begin());
  if (!s.empty() && s.back() == '$') s.pop_back();
  // (?:-\d{4}-\d{2}-\d{2})? — OpenAI date suffix.
  constexpr std::string_view kDateY = "(?:-\\d{4}-\\d{2}-\\d{2})?";
  constexpr std::string_view kDate8 = "(?:-\\d{8})?";
  if (s.size() >= kDateY.size() &&
      s.compare(s.size() - kDateY.size(), kDateY.size(), kDateY) == 0) {
    s.resize(s.size() - kDateY.size());
  }
  if (s.size() >= kDate8.size() &&
      s.compare(s.size() - kDate8.size(), kDate8.size(), kDate8) == 0) {
    s.resize(s.size() - kDate8.size());
  }
  // If anything regex-y remains, skip — the picker won't synthesize this
  // entry on the WebUI side either, so the catalog stays in sync.
  if (s.find_first_of("()[]{}|?+*") != std::string::npos) {
    return std::string();
  }
  // Unescape `\.` etc.
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      out.push_back(s[i + 1]);
      ++i;
    } else {
      out.push_back(s[i]);
    }
  }
  return out;
}

bool IsProviderVerified(PrefService* prefs, const std::string& provider_id) {
  const base::DictValue* config = GetStoredProviderConfig(prefs, provider_id);
  if (!config) {
    return false;
  }

  const base::DictValue* last_verification =
      config->FindDict("last_verification");
  if (!last_verification) {
    return false;
  }

  if (const std::string* verified_provider_id =
          last_verification->FindString("providerId");
      verified_provider_id && !verified_provider_id->empty() &&
      *verified_provider_id != provider_id) {
    return false;
  }

  return last_verification->Find("verifiedAtMs") != nullptr;
}

// Resolve a `{ "h": "a|b|c", "args": { "enabledToolNames": [...] } }`
// system-prompt holder element into text. Splits the holder list on '|',
// resolves each name via the prompt-section renderer (the browser owns
// this prose — e.g. the `using-your-tools` catalog), and joins non-empty
// pieces with "\n\n" — byte-identical to how the JS guest used to inline
// the rendered catalog, but without the per-turn render round-trip.
// Unknown holder names render empty (skipped), matching the renderer's
// contract; section-holder names nexus_web hasn't migrated yet simply
// don't appear as holders, so they keep flowing as literal text.
// Build the prompt-registry resolution condition from the request: provider
// from the resolved provider id, locale from the request's system-context
// (already carried for the model).
HolderCondition BuildHolderCondition(
    const mojom::NexusLlmRequestParams& request,
    const std::string& provider) {
  HolderCondition condition;
  condition.provider = provider;
  if (request.system_context.has_value()) {
    if (const std::string* locale =
            request.system_context->FindString("locale")) {
      condition.locale = *locale;
    }
  }
  return condition;
}

std::string ResolveHolderElement(const base::DictValue& holder,
                                 const HolderCondition& condition) {
  const std::string* h = holder.FindString("h");
  if (!h || h->empty()) {
    return std::string();
  }
  std::vector<std::string> enabled_tool_names;
  if (const base::DictValue* args = holder.FindDict("args")) {
    if (const base::ListValue* names = args->FindList("enabledToolNames")) {
      for (const base::Value& value : *names) {
        if (const std::string* name = value.GetIfString()) {
          enabled_tool_names.push_back(*name);
        }
      }
    }
  }
  // Optional carried default — the literal content nexus_web ships for base
  // prose sections so a single-name holder stays byte-parity when neither a
  // registry override nor the built-in renderer produces text.
  const std::string* fallback = holder.FindString("default");
  std::vector<std::string> names;
  for (std::string_view name : base::SplitStringPiece(
           *h, "|", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    names.emplace_back(name);
  }
  std::vector<std::string> parts;
  for (const std::string& name : names) {
    // Central management: a (provider, holder) override registered by
    // nexus_web wins; then the built-in renderer (catalog / system-prompt-
    // tools); then the carried default (base prose); else the holder is
    // dropped (unknown holder, matching the renderer's contract).
    std::optional<std::string> override_text =
        PromptHolderRegistry::Get().ResolveOverride(name, condition);
    std::string text = override_text.has_value()
                           ? *override_text
                           : RenderPromptSection(name, enabled_tool_names);
    if (text.empty() && names.size() == 1 && fallback) {
      text = *fallback;
    }
    // Every emitted holder must be resolvable by SOME path: a registered
    // override, a known built-in renderer section (which may legitimately
    // render empty when its tools are gated off), or a carried `default`. A
    // name matching none would be silently dropped from the system prompt —
    // almost always a typo or a holder nexus_web emitted without shipping its
    // prose. (Multi-name `a|b` lists carry no `default`, so each name there
    // must resolve via a registered/known section.)
    DCHECK((fallback && names.size() == 1) || IsKnownPromptSection(name) ||
           override_text.has_value())
        << "unresolvable prompt holder '" << name
        << "': no carried default, not a known renderer section, and no "
           "registered override; it would be dropped from the system prompt";
    if (!text.empty()) {
      parts.push_back(std::move(text));
    }
  }
  return base::JoinString(parts, "\n\n");
}

std::string BuildSystemInstruction(
    const mojom::NexusLlmRequestParams& request,
    const std::string& provider) {
  const HolderCondition condition = BuildHolderCondition(request, provider);
  std::vector<std::string> sections;
  if (request.system_prompt.has_value()) {
    // nexus_web sends systemPrompt as an array of independent prompt
    // blocks (role / tools / style / memory / ...). Push each element as
    // its own section so the outer "\n\n" join below preserves paragraph
    // breaks between them. Flattening the whole array into one section
    // here would glue blocks with single newlines (FlattenTextValue's
    // join behavior), which suppresses Markdown headings at block
    // boundaries.
    const base::Value& sp = request.system_prompt.value();
    if (const base::ListValue* list = sp.GetIfList()) {
      for (const base::Value& entry : *list) {
        // A `{ "h": ... }` element is a holder the browser LLM module
        // resolves from its prompt registry; anything else is literal text.
        const base::DictValue* holder = entry.GetIfDict();
        const std::string flattened =
            (holder && holder->contains("h"))
                ? ResolveHolderElement(*holder, condition)
                : FlattenTextValue(&entry);
        if (!flattened.empty()) {
          sections.push_back(flattened);
        }
      }
    } else {
      const std::string flattened = FlattenTextValue(&sp);
      if (!flattened.empty()) {
        sections.push_back(flattened);
      }
    }
  }
  if (request.user_context.has_value()) {
    const std::string section =
        FormatContextSection("User context", &request.user_context.value());
    if (!section.empty()) {
      sections.push_back(section);
    }
  }
  if (request.system_context.has_value()) {
    const std::string section =
        FormatContextSection("System context", &request.system_context.value());
    if (!section.empty()) {
      sections.push_back(section);
    }
  }
  if (request.tool_use_context.has_value()) {
    const std::string section =
        FormatContextSection("Tool use context", &request.tool_use_context.value());
    if (!section.empty()) {
      sections.push_back(section);
    }
  }

  std::string instruction;
  for (const std::string& section : sections) {
    if (section.empty()) {
      continue;
    }
    if (!instruction.empty()) {
      instruction.append("\n\n");
    }
    instruction.append(section);
  }
  return instruction;
}

base::Value ParseToolPayloadValue(const base::Value& value) {
  if (const std::string* text = value.GetIfString()) {
    if (std::optional<base::Value> parsed = ParseJsonValue(*text)) {
      return std::move(*parsed);
    }
    return base::Value(*text);
  }
  return value.Clone();
}

base::DictValue BuildTextPart(const std::string& text) {
  base::DictValue part;
  part.Set("type", "text");
  part.Set("text", text);
  return part;
}

std::string RoleToString(mojom::NexusLlmRole role) {
  switch (role) {
    case mojom::NexusLlmRole::kSystem:
      return "system";
    case mojom::NexusLlmRole::kUser:
      return "user";
    case mojom::NexusLlmRole::kAssistant:
      return "assistant";
    case mojom::NexusLlmRole::kTool:
      return "tool";
  }
  return "user";
}

base::ListValue TranslateBridgeParts(
    const std::vector<mojom::NexusLlmContentPartPtr>& raw_parts,
    ToolNameMap* tool_names) {
  base::ListValue translated;
  for (const auto& part : raw_parts) {
    if (!part) {
      continue;
    }

    switch (part->type) {
      case mojom::NexusLlmContentPartType::kText:
        translated.Append(BuildTextPart(part->text.value_or(std::string())));
        break;

      case mojom::NexusLlmContentPartType::kImage: {
        if (!part->image.has_value() || part->image->empty()) {
          break;
        }
        base::DictValue translated_part;
        translated_part.Set("type", "image");
        translated_part.Set("image", *part->image);
        if (part->mime_type.has_value() && !part->mime_type->empty()) {
          translated_part.Set("mimeType", *part->mime_type);
        }
        translated.Append(std::move(translated_part));
        break;
      }

      case mojom::NexusLlmContentPartType::kToolUse: {
        base::DictValue translated_part;
        translated_part.Set("type", "tool-call");
        if (part->tool_use_id.has_value() && !part->tool_use_id->empty()) {
          translated_part.Set("toolCallId", *part->tool_use_id);
        }
        if (part->name.has_value() && !part->name->empty()) {
          translated_part.Set("toolName", *part->name);
          if (part->tool_use_id.has_value() && !part->tool_use_id->empty()) {
            (*tool_names)[*part->tool_use_id] = *part->name;
          }
        }
        if (part->input.has_value()) {
          translated_part.Set("args", part->input->Clone());
        } else {
          translated_part.Set("args", base::DictValue());
        }
        translated.Append(std::move(translated_part));
        break;
      }

      case mojom::NexusLlmContentPartType::kToolResult: {
        base::DictValue translated_part;
        translated_part.Set("type", "tool-result");

        std::string tool_call_id = part->tool_use_id.value_or(std::string());
        if (!tool_call_id.empty()) {
          translated_part.Set("toolCallId", tool_call_id);
        }

        std::string tool_name = part->name.value_or(std::string());
        if (tool_name.empty() && !tool_call_id.empty()) {
          auto it = tool_names->find(tool_call_id);
          if (it != tool_names->end()) {
            tool_name = it->second;
          }
        }
        if (!tool_name.empty()) {
          translated_part.Set("toolName", tool_name);
        }

        if (part->result.has_value()) {
          translated_part.Set("result", ParseToolPayloadValue(*part->result));
        } else {
          translated_part.Set("result", base::Value(std::string()));
        }

        if (part->is_error.has_value()) {
          translated_part.Set("isError", *part->is_error);
        }

        translated.Append(std::move(translated_part));
        break;
      }

      case mojom::NexusLlmContentPartType::kThinking:
      case mojom::NexusLlmContentPartType::kRedactedThinking:
        break;
    }
  }
  return translated;
}

bool HasOnlyToolResultParts(const base::ListValue& parts) {
  if (parts.empty()) {
    return false;
  }
  for (const auto& value : parts) {
    const base::DictValue* part = value.GetIfDict();
    const std::string* type = part ? part->FindString("type") : nullptr;
    if (!type || *type != "tool-result") {
      return false;
    }
  }
  return true;
}

base::ListValue TranslateBridgeMessages(
    const std::vector<mojom::NexusLlmMessagePtr>& raw_messages) {
  base::ListValue translated;
  ToolNameMap tool_names;
  for (const auto& message : raw_messages) {
    if (!message) {
      continue;
    }

    std::string role = RoleToString(message->role);
    base::ListValue content = TranslateBridgeParts(message->content, &tool_names);
    if (content.empty()) {
      continue;
    }
    if (role == "user" && HasOnlyToolResultParts(content)) {
      role = "tool";
    }

    base::DictValue translated_message;
    translated_message.Set("role", role);
    translated_message.Set("content", std::move(content));
    translated.Append(std::move(translated_message));
  }
  return translated;
}

base::ListValue TranslateBridgeTools(
    const std::vector<mojom::NexusLlmToolPtr>& raw_tools) {
  base::ListValue translated;
  for (const auto& tool : raw_tools) {
    if (!tool || tool->name.empty()) {
      continue;
    }

    base::DictValue translated_tool;
    translated_tool.Set("type", "function");
    translated_tool.Set("name", tool->name);
    if (tool->description.has_value() && !tool->description->empty()) {
      translated_tool.Set("description", *tool->description);
    }
    translated_tool.Set("parameters", tool->input_schema.Clone());
    // Forward the strict-mode opt-in to the per-provider translator.
    // anthropic_provider::TranslateTools and
    // openai_provider::TranslateToolDefinition both read this key and
    // surface it on their wire payload; google/ollama ignore it.
    if (tool->strict) {
      translated_tool.Set("strict", true);
    }
    // Forward optional canonical example calls. Only the Anthropic
    // provider surfaces these on the wire (as `input_examples`); other
    // providers ignore. Stored under the same camelCase key the JS
    // guest used (`inputExamples`) so the per-provider translator
    // doesn't need to special-case naming.
    if (tool->input_examples.has_value()) {
      translated_tool.Set("inputExamples", tool->input_examples->Clone());
    }
    translated.Append(std::move(translated_tool));
  }
  return translated;
}

std::string ResolveProviderIconSvg(const std::string& provider_id) {
  int resource_id = 0;
  if (provider_id == "openai" || provider_id == "chatgpt-pro") {
    // ChatGPT Plus/Pro reuses the OpenAI brand mark — no separate asset.
    resource_id = IDR_CUSTOM_BROWSER_COMPONENTS_NEXUS_OPENAI_SVG;
  } else if (provider_id == "anthropic") {
    resource_id = IDR_CUSTOM_BROWSER_COMPONENTS_NEXUS_ANTHROPIC_SVG;
  } else if (provider_id == "google") {
    resource_id = IDR_CUSTOM_BROWSER_COMPONENTS_NEXUS_GOOGLE_SVG;
  } else if (provider_id == "ollama") {
    resource_id = IDR_CUSTOM_BROWSER_COMPONENTS_NEXUS_OLLAMA_SVG;
  } else if (provider_id == "kimi") {
    resource_id = IDR_CUSTOM_BROWSER_COMPONENTS_NEXUS_KIMI_SVG;
  } else if (provider_id == "minimax") {
    resource_id = IDR_CUSTOM_BROWSER_COMPONENTS_NEXUS_MINIMAX_SVG;
  } else if (provider_id == "qwen" || provider_id == "qwen-code") {
    // Qwen Code (OAuth) reuses the Qwen brand mark.
    resource_id = IDR_CUSTOM_BROWSER_COMPONENTS_NEXUS_QWEN_SVG;
  } else if (provider_id == "github-copilot") {
    resource_id = IDR_CUSTOM_BROWSER_COMPONENTS_NEXUS_GITHUBCOPILOT_SVG;
  } else if (provider_id == "glm") {
    resource_id = IDR_CUSTOM_BROWSER_COMPONENTS_NEXUS_GLM_SVG;
  } else if (provider_id == "deepseek") {
    resource_id = IDR_CUSTOM_BROWSER_COMPONENTS_NEXUS_DEEPSEEK_SVG;
  } else if (provider_id.starts_with("custom-")) {
    // Every custom endpoint — the two compiled-in dialects and any provider
    // the user created — shares one neutral cube mark; no vendor brand applies
    // to an arbitrary endpoint. (The guest overrides this with the icon the
    // user picked; this is what the browser-side settings UI shows.)
    resource_id = IDR_CUSTOM_BROWSER_COMPONENTS_NEXUS_CUSTOM_SVG;
  }

  if (resource_id != 0 && ui::ResourceBundle::HasSharedInstance()) {
    std::string icon_svg =
        ui::ResourceBundle::GetSharedInstance().LoadDataResourceString(
            resource_id);
    if (!icon_svg.empty()) {
      return icon_svg;
    }
  }
  return R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><rect width="24" height="24" rx="12" fill="#475569"/><text x="12" y="15" text-anchor="middle" font-size="11" font-family="Arial, sans-serif" fill="#ffffff">L</text></svg>)";
}

std::string ResolveEnabledModelId(const mojom::NexusLlmProvider& provider) {
  if (provider.default_model_id.has_value() &&
      !provider.default_model_id->empty()) {
    for (const auto& model : provider.models) {
      if (model && model->id == *provider.default_model_id && model->enabled) {
        return model->id;
      }
    }
  }
  for (const auto& model : provider.models) {
    if (model && !model->id.empty() && model->enabled) {
      return model->id;
    }
  }
  return std::string();
}

void MaybeSetDefaultSelection(mojom::NexusLlmCatalog* catalog,
                              const std::string& active_provider_id) {
  if (!catalog) {
    return;
  }

  const mojom::NexusLlmProvider* selected_provider = nullptr;
  if (!active_provider_id.empty()) {
    for (const auto& provider : catalog->providers) {
      if (provider && provider->id == active_provider_id && provider->enabled) {
        selected_provider = provider.get();
        break;
      }
    }
  }
  if (!selected_provider) {
    for (const auto& provider : catalog->providers) {
      if (provider && provider->enabled) {
        selected_provider = provider.get();
        break;
      }
    }
  }
  if (!selected_provider) {
    return;
  }

  const std::string model_id = ResolveEnabledModelId(*selected_provider);
  if (selected_provider->id.empty() || model_id.empty()) {
    return;
  }

  auto selection = mojom::NexusLlmSelection::New();
  selection->provider_id = selected_provider->id;
  selection->model_id = model_id;
  catalog->default_selection = std::move(selection);
}

mojom::NexusLlmCatalogPtr BuildCatalog(Profile* profile) {
  auto catalog = mojom::NexusLlmCatalog::New();

  LlmBroker* broker = profile ? LlmBrokerFactory::GetForProfile(profile) : nullptr;
  PrefService* prefs = profile ? profile->GetPrefs() : nullptr;
  if (!broker) {
    return catalog;
  }

  // Index the TS-pushed catalog by providerId so we can graft its
  // curated models into providers whose own `info.models` came back
  // empty (the common case after we removed `RefreshAllCatalogs` —
  // `GetSupportedModels()` returns nothing). Without this, the worker's
  // `isSelectionEnabled` gate (BrowserRuntimeWorker.ts:73) refuses
  // every pick the WebUI quick-settings popup makes because no model
  // is "enabled" in the bridge catalog the worker consumes.
  const base::DictValue& ts_catalog = broker->GetTsModelCatalog();
  std::map<std::string, const base::DictValue*> ts_by_provider;
  if (const base::ListValue* providers = ts_catalog.FindList("providers")) {
    for (const base::Value& entry : *providers) {
      const base::DictValue* p = entry.GetIfDict();
      if (!p) continue;
      if (const std::string* pid = p->FindString("providerId")) {
        ts_by_provider[*pid] = p;
      }
    }
  }

  for (const ProviderInfo& info : broker->GetRegisteredProviders()) {
    LlmProvider* provider = broker->GetProvider(info.id);
    // Cold-boot race: the broker's LoadProviderConfigs() runs during
    // LlmBroker construction, which can precede the profile's Preferences
    // file being parsed into the PrefService. When that happens the
    // in-memory provider never gets Configure()d, so IsConfigured() is
    // false here even though a valid api_key sits in prefs — and the
    // catalog lists the provider as disabled. Re-apply the stored config
    // now (prefs is fully loaded by the time BuildCatalog runs) so a
    // late-loading prefs file still yields a configured provider.
    if (provider && !provider->IsConfigured() && prefs) {
      if (const base::DictValue* stored_config =
              GetStoredProviderConfig(prefs, info.id)) {
        provider->Configure(*stored_config);
      }
    }
    // For OAuth providers, treat "signed in" (tokens present) as the source of
    // truth for configured/verified — the in-memory api_key or the prefs
    // last_verification stamp can lag (e.g. tokens stored before stamping
    // existed), which would otherwise hide the provider from the picker even
    // though it works.
    const bool oauth_connected =
        IsOAuthProvider(info.id) && broker->oauth_manager() &&
        broker->oauth_manager()->IsConnected(info.id);
    const bool provider_configured =
        (provider && provider->IsConfigured()) || oauth_connected;
    const bool provider_verified =
        provider_configured &&
        (oauth_connected || IsProviderVerified(prefs, info.id));

    bool has_enabled_model = false;
    std::vector<mojom::NexusLlmModelPtr> models;
    std::set<std::string> seen_ids;
    for (const ModelInfo& model : info.models) {
      const bool model_enabled =
          provider_verified && IsModelEnabled(prefs, info.id, model.id);
      has_enabled_model = has_enabled_model || model_enabled;

      auto model_ptr = mojom::NexusLlmModel::New();
      model_ptr->id = model.id;
      model_ptr->label = model.name.empty() ? model.id : model.name;
      model_ptr->enabled = model_enabled;
      model_ptr->supports_streaming =
          provider ? provider->SupportsStreaming() : false;
      model_ptr->supports_tools =
          model.supports_tools || (provider ? provider->SupportsTools() : false);
      // Reasoning-effort metadata from dynamic discovery (e.g. the Codex
      // /models endpoint). Lets the guest render the thinking-tier picker
      // without a hardcoded curated entry.
      if (model.default_thinking_tier &&
          !model.default_thinking_tier->empty()) {
        model_ptr->default_thinking_tier = *model.default_thinking_tier;
      }
      model_ptr->available_thinking_tiers = model.available_thinking_tiers;
      seen_ids.insert(model.id);
      models.push_back(std::move(model_ptr));
    }

    // Always splice in TS-curated models — the popup picks from this
    // list, so the worker's `isSelectionEnabled` gate must accept every
    // id the popup advertises. Previously this graft only ran when
    // `info.models` was empty, which silently dropped curated entries
    // for any provider whose `/v1/models` returned a non-empty list
    // that didn't already contain the curated ids (e.g. an OpenAI
    // endpoint that doesn't yet list gpt-5.5, or a Gemini endpoint that
    // names models with a `models/` prefix). Mirrors `deriveModelId` on
    // the WebUI side so the ids the picker sends match the ids the
    // worker accepts.
    auto it = ts_by_provider.find(info.id);
    if (it != ts_by_provider.end() && it->second) {
      if (const base::ListValue* ts_models =
              it->second->FindList("models")) {
        for (const base::Value& m_val : *ts_models) {
          const base::DictValue* m = m_val.GetIfDict();
          if (!m) continue;
          const std::string* pattern = m->FindString("model");
          if (!pattern) continue;
          std::string id = DeriveTsModelId(*pattern);
          if (id.empty()) continue;
          // Skip ids already added from `info.models` so we don't
          // duplicate. The worker only inspects id + enabled, so we
          // keep the first entry as-is.
          if (!seen_ids.insert(id).second) continue;

          // Honour the user's per-provider enabled-models allowlist
          // when present; if absent or empty, all curated models are
          // enabled for this verified provider.
          const bool model_enabled =
              provider_verified && IsModelEnabled(prefs, info.id, id);
          has_enabled_model = has_enabled_model || model_enabled;

          auto model_ptr = mojom::NexusLlmModel::New();
          model_ptr->id = id;
          const std::string* name = m->FindString("name");
          model_ptr->label = (name && !name->empty()) ? *name : id;
          model_ptr->enabled = model_enabled;
          model_ptr->supports_streaming =
              provider ? provider->SupportsStreaming() : false;
          model_ptr->supports_tools =
              provider ? provider->SupportsTools() : false;
          models.push_back(std::move(model_ptr));
        }
      }
    }

    const base::DictValue* stored_config_for_model =
        GetStoredProviderConfig(prefs, info.id);
    std::string default_model_id =
        stored_config_for_model
            ? GetStoredDefaultModelId(*stored_config_for_model)
            : std::string();
    if (default_model_id.empty() && provider) {
      default_model_id = provider->GetDefaultModelId();
    }

    // Ensure the configured default_model is always selectable. Two cases:
    // 1. Cold boot with no online fetch and no curated graft → the catalog
    //    has zero models; synthesize the default so the provider isn't
    //    empty (the original cold-boot fallback).
    // 2. Cold boot with curated models but no online fetch → the catalog
    //    has curated models (e.g. claude-fable-5) but NOT the user's
    //    configured default_model (e.g. claude-sonnet-4-6 from the fake
    //    server's /v1/models). Without this, setModelSelection to the
    //    configured default fails with worker_invalid_model_selection.
    // Skip if the id is already present (from online fetch or curated
    // graft) to avoid duplicates.
    if (provider_verified && !default_model_id.empty() &&
        seen_ids.find(default_model_id) == seen_ids.end()) {
      auto model_ptr = mojom::NexusLlmModel::New();
      model_ptr->id = default_model_id;
      model_ptr->label = default_model_id;
      model_ptr->enabled = true;
      model_ptr->supports_streaming =
          provider ? provider->SupportsStreaming() : false;
      model_ptr->supports_tools =
          provider ? provider->SupportsTools() : false;
      has_enabled_model = true;
      models.push_back(std::move(model_ptr));
    }

    auto provider_ptr = mojom::NexusLlmProvider::New();
    provider_ptr->id = info.id;
    provider_ptr->label = info.name;
    provider_ptr->icon_svg = ResolveProviderIconSvg(info.id);
    provider_ptr->enabled = provider_verified && has_enabled_model;
    if (IsOAuthProvider(info.id)) {
      DLOG(INFO) << "[nexus-catalog] " << info.id
                 << " configured=" << provider_configured
                 << " verified=" << provider_verified
                 << " fetched_models=" << info.models.size()
                 << " has_enabled_model=" << has_enabled_model
                 << " enabled=" << provider_ptr->enabled;
    }
    if (!default_model_id.empty()) {
      provider_ptr->default_model_id = default_model_id;
    }
    provider_ptr->models = std::move(models);
    catalog->providers.push_back(std::move(provider_ptr));

    if (provider) {
      std::string raw_json = provider->GetLastFetchedRawCatalogJson();
      if (!raw_json.empty()) {
        catalog->raw_catalogs.emplace(info.id, std::move(raw_json));
      }
    }
  }

  MaybeSetDefaultSelection(catalog.get(), broker->GetActiveProviderId());
  return catalog;
}

LlmBroker* GetBroker(Profile* profile) {
  return profile ? LlmBrokerFactory::GetForProfile(profile) : nullptr;
}

bool ProviderHasModelId(const ProviderInfo& provider_info,
                        const std::string& model_id) {
  for (const ModelInfo& model : provider_info.models) {
    if (model.id == model_id) {
      return true;
    }
  }
  return false;
}

LlmProvider* ResolveProvider(Profile* profile,
                             const mojom::NexusLlmRequestParams& request,
                             std::string* provider_id_out) {
  LlmBroker* broker = GetBroker(profile);
  if (!broker) {
    return nullptr;
  }

  if (request.provider_id.has_value() && !request.provider_id->empty()) {
    LlmProvider* provider = broker->GetProvider(*request.provider_id);
    if (provider) {
      *provider_id_out = *request.provider_id;
      return provider;
    }
    return nullptr;
  }

  const std::string active_provider_id = broker->GetActiveProviderId();
  if (!active_provider_id.empty()) {
    LlmProvider* provider = broker->GetProvider(active_provider_id);
    if (provider && provider->IsConfigured()) {
      *provider_id_out = active_provider_id;
      return provider;
    }
  }

  if (request.model_id.has_value() && !request.model_id->empty()) {
    for (const ProviderInfo& info : broker->GetRegisteredProviders()) {
      LlmProvider* provider = broker->GetProvider(info.id);
      if (!provider || !provider->IsConfigured()) {
        continue;
      }
      for (const ModelInfo& model : info.models) {
        if (model.id == *request.model_id) {
          *provider_id_out = info.id;
          return provider;
        }
      }
    }
  }

  for (const ProviderInfo& info : broker->GetRegisteredProviders()) {
    LlmProvider* provider = broker->GetProvider(info.id);
    if (provider && provider->IsConfigured()) {
      *provider_id_out = info.id;
      return provider;
    }
  }
  return nullptr;
}

std::string ResolveModelId(Profile* profile,
                           const mojom::NexusLlmRequestParams& request,
                           const std::string& provider_id,
                           LlmProvider* provider) {
  // "inherit" is the guest's sentinel for "no explicit pick — use the
  // provider's configured default". Treat it the same as an absent/empty
  // model_id so the fall-through below resolves default_model → model →
  // provider default. Without this, the literal string "inherit" would be
  // sent to the upstream API as the model name.
  if (request.model_id.has_value() && !request.model_id->empty() &&
      *request.model_id != "inherit") {
    return *request.model_id;
  }
  const base::DictValue* stored_config =
      profile ? GetStoredProviderConfig(profile->GetPrefs(), provider_id) : nullptr;
  if (stored_config) {
    if (const auto* default_model = stored_config->FindString("default_model");
        default_model && !default_model->empty()) {
      return *default_model;
    }
    if (const auto* model = stored_config->FindString("model");
        model && !model->empty()) {
      return *model;
    }
  }
  return provider ? provider->GetDefaultModelId() : std::string();
}

base::DictValue BuildCallOptions(
    Profile* profile,
    const mojom::NexusLlmRequestParams& request,
                                 const std::string& provider_id,
                                 LlmProvider* provider,
                                 const std::string& request_id) {
  base::DictValue call_options;
  call_options.Set("requestId", request_id);

  const std::string model_id =
      ResolveModelId(profile, request, provider_id, provider);
  if (!model_id.empty()) {
    call_options.Set("model", model_id);
    call_options.Set("modelId", model_id);
  }

  base::ListValue messages = TranslateBridgeMessages(request.messages);

  const std::string system_instruction =
      BuildSystemInstruction(request, provider_id);
  if (!system_instruction.empty()) {
    base::ListValue prefixed_messages;
    base::DictValue system_message;
    system_message.Set("role", "system");
    base::ListValue content;
    content.Append(BuildTextPart(system_instruction));
    system_message.Set("content", std::move(content));
    prefixed_messages.Append(std::move(system_message));
    for (auto& message : messages) {
      prefixed_messages.Append(std::move(message));
    }
    messages = std::move(prefixed_messages);
  }

  call_options.Set("inputFormat", "messages");
  call_options.Set("messages", messages.Clone());
  call_options.Set("prompt", std::move(messages));

  const base::ListValue translated_tools = TranslateBridgeTools(request.tools);
  if (!translated_tools.empty()) {
    base::DictValue mode;
    mode.Set("type", "regular");
    mode.Set("tools", translated_tools.Clone());

    base::DictValue tool_choice_dict;
    tool_choice_dict.Set("type", kToolChoiceAuto);
    mode.Set("toolChoice", tool_choice_dict.Clone());

    call_options.Set("mode", std::move(mode));
    call_options.Set("tools", translated_tools.Clone());
    call_options.Set("toolChoice", tool_choice_dict.Clone());
    call_options.Set("tool_choice", std::string(kToolChoiceAuto));
  } else {
    base::DictValue mode;
    mode.Set("type", "regular");
    call_options.Set("mode", std::move(mode));
  }

  if (request.max_output_tokens.has_value()) {
    call_options.Set("maxTokens", *request.max_output_tokens);
    call_options.Set("max_tokens", *request.max_output_tokens);
  }
  if (request.provider_body_extension.has_value()) {
    call_options.Set("providerBodyExtension",
                     request.provider_body_extension->Clone());
  }

  return call_options;
}

FinishReason ParseFinishReasonString(const std::string& reason) {
  std::string normalized = base::ToLowerASCII(reason);
  base::ReplaceChars(normalized, "-", "_", &normalized);

  if (normalized == "stop" || normalized == "end_turn") {
    return FinishReason::kStop;
  }
  if (normalized == "length" || normalized == "max_tokens") {
    return FinishReason::kLength;
  }
  if (normalized == "content_filter" || normalized == "contentfiltered") {
    return FinishReason::kContentFilter;
  }
  if (normalized == "tool_calls" || normalized == "tool_use") {
    return FinishReason::kToolCalls;
  }
  if (normalized == "error") {
    return FinishReason::kError;
  }
  if (normalized.empty()) {
    return FinishReason::kUnknown;
  }
  return FinishReason::kOther;
}

mojom::NexusLlmFinishReason ToMojoFinishReason(FinishReason reason) {
  switch (reason) {
    case FinishReason::kStop:
      return mojom::NexusLlmFinishReason::kStop;
    case FinishReason::kLength:
      return mojom::NexusLlmFinishReason::kLength;
    case FinishReason::kContentFilter:
      return mojom::NexusLlmFinishReason::kContentFilter;
    case FinishReason::kToolCalls:
      return mojom::NexusLlmFinishReason::kToolCalls;
    case FinishReason::kError:
      return mojom::NexusLlmFinishReason::kError;
    case FinishReason::kOther:
      return mojom::NexusLlmFinishReason::kOther;
    case FinishReason::kUnknown:
      return mojom::NexusLlmFinishReason::kUnknown;
  }
  return mojom::NexusLlmFinishReason::kUnknown;
}

mojom::NexusLlmUsagePtr BuildUsage(int prompt_tokens,
                                   int completion_tokens,
                                   int cache_read_input_tokens = 0,
                                   int cache_creation_input_tokens = 0) {
  auto usage = mojom::NexusLlmUsage::New();
  usage->prompt_tokens = prompt_tokens;
  usage->completion_tokens = completion_tokens;
  usage->cache_read_input_tokens = cache_read_input_tokens;
  usage->cache_creation_input_tokens = cache_creation_input_tokens;
  return usage;
}

mojom::NexusLlmUsagePtr UsageFromProviderDict(const base::DictValue* usage) {
  if (!usage) {
    return nullptr;
  }
  return BuildUsage(
      usage->FindInt("promptTokens").value_or(0),
      usage->FindInt("completionTokens").value_or(0),
      usage->FindInt("cacheReadInputTokens").value_or(0),
      usage->FindInt("cacheCreationInputTokens").value_or(0));
}

mojom::NexusLlmContentPartPtr BuildReasoningPart(const base::DictValue& reasoning) {
  auto part = mojom::NexusLlmContentPart::New();
  part->type = mojom::NexusLlmContentPartType::kThinking;
  if (const auto* text = reasoning.FindString("text"); text && !text->empty()) {
    part->text = *text;
  } else if (const auto* thinking = reasoning.FindString("thinking");
             thinking && !thinking->empty()) {
    part->text = *thinking;
  } else {
    part->text = SerializeValueToJson(base::Value(reasoning.Clone()));
  }
  if (const auto* signature = reasoning.FindString("signature");
      signature && !signature->empty()) {
    part->signature = *signature;
  }
  return part;
}

mojom::NexusLlmAssistantMessagePtr BuildAssistantMessage(
    const std::string& model_id,
    const GenerateResult& result) {
  auto message = mojom::NexusLlmAssistantMessage::New();

  if (result.reasoning) {
    for (const auto& reasoning : *result.reasoning) {
      message->content.push_back(BuildReasoningPart(reasoning));
    }
  }

  if (result.text && !result.text->empty()) {
    auto text_part = mojom::NexusLlmContentPart::New();
    text_part->type = mojom::NexusLlmContentPartType::kText;
    text_part->text = *result.text;
    message->content.push_back(std::move(text_part));
  }

  if (result.tool_calls) {
    for (const auto& call : *result.tool_calls) {
      auto part = mojom::NexusLlmContentPart::New();
      part->type = mojom::NexusLlmContentPartType::kToolUse;
      if (const auto* tool_call_id = call.FindString("toolCallId");
          tool_call_id && !tool_call_id->empty()) {
        part->tool_use_id = *tool_call_id;
      }
      if (const auto* tool_name = call.FindString("toolName");
          tool_name && !tool_name->empty()) {
        part->name = *tool_name;
      }
      if (const auto* args = call.Find("args")) {
        part->input = args->Clone();
      } else {
        part->input = base::Value(base::DictValue());
      }
      message->content.push_back(std::move(part));
    }
  }

  if (message->content.empty()) {
    auto text_part = mojom::NexusLlmContentPart::New();
    text_part->type = mojom::NexusLlmContentPartType::kText;
    text_part->text = std::string();
    message->content.push_back(std::move(text_part));
  }

  if (!model_id.empty()) {
    message->model_id = model_id;
  }
  message->finish_reason = ToMojoFinishReason(result.finish_reason);
  message->usage = BuildUsage(result.prompt_tokens,
                              result.completion_tokens,
                              result.cache_read_input_tokens,
                              result.cache_creation_input_tokens);
  return message;
}

base::Value BuildToolUseInput(const std::string& args_json) {
  if (args_json.empty()) {
    return base::Value(base::DictValue());
  }
  if (std::optional<base::Value> parsed = ParseJsonValue(args_json)) {
    return std::move(*parsed);
  }
  return base::Value(args_json);
}

void EnqueueEvent(NexusLlmHost::StreamState* state,
                  mojom::NexusLlmStreamEventPtr event) {
  state->queued_events.push_back(std::move(event));
}

void QueueMessageStartEvent(NexusLlmHost::StreamState* state,
                            mojom::NexusLlmUsagePtr usage = nullptr) {
  auto event = mojom::NexusLlmStreamEvent::New();
  event->type = mojom::NexusLlmStreamEventType::kMessageStart;
  event->model_id = state->model_id;
  if (usage) {
    event->usage = std::move(usage);
  }
  EnqueueEvent(state, std::move(event));
}

void QueueUsageUpdate(NexusLlmHost::StreamState* state,
                      mojom::NexusLlmUsagePtr usage) {
  // Re-emit kMessageStart with the new usage so the bridge surfaces
  // the updated usage on the message_start chunk. Consumers treat
  // successive message_start chunks as a usage refresh (they don't
  // restart the message).
  QueueMessageStartEvent(state, std::move(usage));
}

void QueueCompletedToolCallEvent(NexusLlmHost::StreamState* state,
                                 const std::string& tool_call_id,
                                 const std::string& tool_name,
                                 const std::string& args_json) {
  auto event = mojom::NexusLlmStreamEvent::New();
  event->type = mojom::NexusLlmStreamEventType::kToolCall;
  event->tool_use_id = tool_call_id.empty()
                           ? base::Uuid::GenerateRandomV4().AsLowercaseString()
                           : tool_call_id;
  if (!tool_name.empty()) {
    event->name = tool_name;
  }
  event->input = BuildToolUseInput(args_json);
  EnqueueEvent(state, std::move(event));
}

void FlushPendingToolCalls(NexusLlmHost::StreamState* state) {
  for (const std::string& tool_call_id : state->pending_tool_call_order) {
    auto it = state->pending_tool_calls.find(tool_call_id);
    if (it == state->pending_tool_calls.end()) {
      continue;
    }
    QueueCompletedToolCallEvent(state, it->second.tool_call_id,
                                it->second.tool_name, it->second.args_json);
  }
  state->pending_tool_calls.clear();
  state->pending_tool_call_order.clear();
}

void QueueFinishEvent(NexusLlmHost::StreamState* state,
                      FinishReason reason,
                      const base::DictValue* usage) {
  if (state->terminal_event_queued) {
    return;
  }

  auto event = mojom::NexusLlmStreamEvent::New();
  event->type = mojom::NexusLlmStreamEventType::kFinish;
  event->finish_reason = ToMojoFinishReason(reason);
  mojom::NexusLlmUsagePtr usage_ptr = UsageFromProviderDict(usage);
  event->usage = usage_ptr ? std::move(usage_ptr) : BuildUsage(0, 0);
  EnqueueEvent(state, std::move(event));
  state->terminal_event_queued = true;
}

void QueueErrorEvent(NexusLlmHost::StreamState* state,
                     const std::string& message) {
  if (state->terminal_event_queued) {
    return;
  }

  auto event = mojom::NexusLlmStreamEvent::New();
  event->type = mojom::NexusLlmStreamEventType::kError;
  event->error_message = message;
  EnqueueEvent(state, std::move(event));
  state->terminal_event_queued = true;
}

}  // namespace

namespace {

// Epoch-ms "now" for the timeline stamps the JS guest logs. Uses
// base::Time::Now() (wall clock) because the stamps are compared against
// JS-side Date.now() values — NOT base::TimeTicks (monotonic, different
// epoch).
double NowEpochMs() {
  return static_cast<double>(
      base::Time::Now().InMillisecondsFSinceUnixEpochIgnoringNull());
}

}  // namespace

NexusLlmHost::StreamState::StreamState() = default;
NexusLlmHost::StreamState::StreamState(StreamState&&) = default;
NexusLlmHost::StreamState& NexusLlmHost::StreamState::operator=(StreamState&&) =
    default;
NexusLlmHost::StreamState::~StreamState() = default;

NexusLlmHost::PendingStreamRead::PendingStreamRead() = default;
NexusLlmHost::PendingStreamRead::PendingStreamRead(PendingStreamRead&&) =
    default;
NexusLlmHost::PendingStreamRead& NexusLlmHost::PendingStreamRead::operator=(
    PendingStreamRead&&) = default;
NexusLlmHost::PendingStreamRead::~PendingStreamRead() = default;

NexusLlmHost::PendingCall::PendingCall() = default;
NexusLlmHost::PendingCall::PendingCall(PendingCall&&) = default;
NexusLlmHost::PendingCall& NexusLlmHost::PendingCall::operator=(PendingCall&&) =
    default;
NexusLlmHost::PendingCall::~PendingCall() = default;

NexusLlmHost::NexusLlmHost(Profile* profile) : profile_(profile) {}

NexusLlmHost::~NexusLlmHost() = default;

void NexusLlmHost::Catalog(CatalogCallback callback) {
  LlmBroker* broker = GetBroker(profile_);
  if (!broker) {
    std::move(callback).Run(true, BuildCatalog(profile_), std::nullopt);
    return;
  }

  // Cold-boot race: the broker's `LoadProviderConfigs` runs at broker
  // construction, which can happen BEFORE the profile's Preferences file
  // is loaded into the PrefService. Re-apply stored configs here so a
  // late-loading prefs file still yields configured providers before we
  // build the `configured_provider_ids` list below.
  if (PrefService* prefs = profile_ ? profile_->GetPrefs() : nullptr) {
    for (const ProviderInfo& info : broker->GetRegisteredProviders()) {
      LlmProvider* provider = broker->GetProvider(info.id);
      if (provider && !provider->IsConfigured()) {
        if (const base::DictValue* stored_config =
                GetStoredProviderConfig(prefs, info.id)) {
          provider->Configure(*stored_config);
        }
      }
    }
  }

  std::vector<std::string> configured_provider_ids;
  for (const ProviderInfo& info : broker->GetRegisteredProviders()) {
    // Check the live provider, not the stale `info.is_configured` snapshot —
    // the re-configure loop above may have just flipped it.
    LlmProvider* provider = broker->GetProvider(info.id);
    if (provider && provider->IsConfigured()) {
      configured_provider_ids.push_back(info.id);
    }
  }

  if (configured_provider_ids.empty()) {
    std::move(callback).Run(true, BuildCatalog(profile_), std::nullopt);
    return;
  }

  const bool can_use_cached_catalog =
      !last_catalog_refresh_time_.is_null() &&
      last_catalog_refresh_provider_ids_ == configured_provider_ids &&
      (base::TimeTicks::Now() - last_catalog_refresh_time_) <
          kCatalogRefreshInterval;
  // On cold boot (first call), skip the model-discovery round-trip and
  // return the prefs-backed catalog immediately. The TS-curated catalog
  // graft in BuildCatalog already supplies every model id the picker needs;
  // discovery only refreshes reasoning-tier metadata. This avoids a
  // BarrierClosure that can stall if any provider's /models endpoint is
  // slow or unreachable on first boot.
  if (can_use_cached_catalog || last_catalog_refresh_time_.is_null()) {
    std::move(callback).Run(true, BuildCatalog(profile_), std::nullopt);
    return;
  }

  base::RepeatingClosure on_provider_models_fetched = base::BarrierClosure(
      configured_provider_ids.size(),
      base::BindOnce(
          [](base::WeakPtr<NexusLlmHost> self, CatalogCallback callback,
             std::vector<std::string> refreshed_provider_ids) {
            if (!self) {
              return;
            }

            self->last_catalog_refresh_time_ = base::TimeTicks::Now();
            self->last_catalog_refresh_provider_ids_ =
                std::move(refreshed_provider_ids);
            std::move(callback).Run(true, BuildCatalog(self->profile_),
                                    std::nullopt);
          },
          weak_factory_.GetWeakPtr(), std::move(callback),
          configured_provider_ids));

  for (const std::string& provider_id : configured_provider_ids) {
    broker->RefreshModels(
        provider_id,
        base::BindOnce(
            [](base::RepeatingClosure done,
               std::vector<ModelInfo> /*models*/) { done.Run(); },
            on_provider_models_fetched));
  }
}

bool NexusLlmHost::SetDefaultSelection(const std::string& provider_id,
                                       const std::string& model_id,
                                       std::string* error_message) {
  if (provider_id.empty() || model_id.empty()) {
    if (error_message) {
      *error_message = "Provider and model ids are required";
    }
    return false;
  }

  LlmBroker* broker = GetBroker(profile_);
  PrefService* prefs = profile_ ? profile_->GetPrefs() : nullptr;
  if (!broker || !prefs) {
    if (error_message) {
      *error_message = "Nexus LLM host is not available";
    }
    return false;
  }

  const ProviderInfo* provider_info = nullptr;
  for (const ProviderInfo& info : broker->GetRegisteredProviders()) {
    if (info.id == provider_id) {
      provider_info = &info;
      break;
    }
  }
  if (!provider_info) {
    if (error_message) {
      *error_message = "Unknown provider: " + provider_id;
    }
    return false;
  }

  if (!ProviderHasModelId(*provider_info, model_id)) {
    DLOG(WARNING) << "Persisting Nexus default model not currently present in "
                  << "provider catalog: " << provider_id << "/" << model_id;
  }

  LlmProvider* provider = broker->GetProvider(provider_id);
  if (!provider) {
    if (error_message) {
      *error_message = "Provider is unavailable: " + provider_id;
    }
    return false;
  }

  const base::DictValue* stored_config = GetStoredProviderConfig(prefs, provider_id);
  base::DictValue config = stored_config ? stored_config->Clone() : base::DictValue();
  config.Set("default_model", model_id);
  config.Set("model", model_id);

  if (!broker->ConfigureProvider(provider_id, config)) {
    if (error_message) {
      *error_message = "Failed to configure provider: " + provider_id;
    }
    return false;
  }

  if (!broker->SetActiveProvider(provider_id)) {
    if (error_message) {
      *error_message = "Failed to set active provider: " + provider_id;
    }
    return false;
  }

  return true;
}

void NexusLlmHost::EnsureProviderAuthThen(
    const std::string& provider_id,
    base::OnceCallback<void(bool ok)> done) {
  LlmBroker* broker = GetBroker(profile_);
  if (!broker || !broker->oauth_manager() || !IsOAuthProvider(provider_id)) {
    // Non-OAuth providers (or no manager) need no token refresh.
    std::move(done).Run(true);
    return;
  }

  broker->oauth_manager()->EnsureFreshToken(
      provider_id,
      base::BindOnce(&NexusLlmHost::OnEnsureFreshToken,
                     weak_factory_.GetWeakPtr(), provider_id,
                     std::move(done)));
}

void NexusLlmHost::OnEnsureFreshToken(const std::string& provider_id,
                                      base::OnceCallback<void(bool ok)> done,
                                      bool ok,
                                      std::string access_token,
                                      std::string account_id) {
  if (ok) {
    // Apply the fresh token to the provider in-memory only — the token store
    // already persisted it; calling broker->ConfigureProvider() with a
    // partial dict would clobber default_model et al. in prefs.
    if (LlmBroker* broker = GetBroker(profile_)) {
      if (LlmProvider* provider = broker->GetProvider(provider_id)) {
        base::DictValue config;
        config.Set("api_key", access_token);
        config.Set("account_id", account_id);
        provider->Configure(config);
      }
    }
  }
  std::move(done).Run(ok);
}

void NexusLlmHost::Call(mojom::NexusLlmRequestParamsPtr request,
                        std::optional<double> deadline_ms,
                        base::DictValue timeline,
                        CallCallback callback) {
  if (!request) {
    std::move(callback).Run(false, nullptr,
                            std::make_optional("Missing LLM request"));
    return;
  }

  std::string provider_id;
  LlmProvider* provider = ResolveProvider(profile_, *request, &provider_id);
  if (!provider) {
    std::move(callback).Run(
        false, nullptr,
        std::make_optional(
            "No Nexus LLM provider is available for the requested call"));
    return;
  }
  if (!provider->IsConfigured()) {
    std::move(callback).Run(
        false, nullptr,
        std::make_optional("Requested Nexus LLM provider is not configured"));
    return;
  }

  timeline.Set("t_host_read", NowEpochMs());
  EnsureProviderAuthThen(
      provider_id,
      base::BindOnce(&NexusLlmHost::ContinueCall, weak_factory_.GetWeakPtr(),
                     std::move(request), deadline_ms, std::move(timeline),
                     std::move(callback)));
}

void NexusLlmHost::ContinueCall(mojom::NexusLlmRequestParamsPtr request,
                                std::optional<double> deadline_ms,
                                base::DictValue timeline,
                                CallCallback callback,
                                bool auth_ok) {
  if (!auth_ok) {
    std::move(callback).Run(
        false, nullptr,
        std::make_optional(
            "Sign in to this provider in Nexus settings to continue"));
    return;
  }

  std::string provider_id;
  LlmProvider* provider = ResolveProvider(profile_, *request, &provider_id);
  if (!provider || !provider->IsConfigured()) {
    std::move(callback).Run(
        false, nullptr,
        std::make_optional("Requested Nexus LLM provider is not configured"));
    return;
  }

  const std::string request_id =
      base::Uuid::GenerateRandomV4().AsLowercaseString();
  const base::DictValue call_options =
      BuildCallOptions(profile_, *request, provider_id, provider, request_id);
  const std::string resolved_model_id =
      ResolveModelId(profile_, *request, provider_id, provider);

  // Track the in-flight call so OnCallDeadlineTimeout can find + cancel it,
  // and so OnGenerateComplete can stop the deadline timer before resolving.
  PendingCall pending;
  pending.resolved_model_id = resolved_model_id;
  pending.callback = std::move(callback);
  pending.timeline = std::move(timeline);
  if (deadline_ms.has_value()) {
    const double now_ms = NowEpochMs();
    const double remaining_ms = *deadline_ms - now_ms;
    if (remaining_ms <= 0) {
      // Already past the deadline — cancel upstream and resolve as timeout
      // without ever dispatching to the provider.
      if (LlmBroker* broker = GetBroker(profile_)) {
        broker->CancelRequest(request_id);
      }
      pending.timeline.Set("t_resolve", now_ms);
      auto response = mojom::NexusLlmCallResult::New();
      response->status = "timeout";
      response->timeline = std::move(pending.timeline);
      std::move(pending.callback).Run(true, std::move(response), std::nullopt);
      return;
    }
    auto timer = std::make_unique<base::OneShotTimer>();
    timer->Start(FROM_HERE, base::Milliseconds(static_cast<int64_t>(remaining_ms)),
                 base::BindOnce(&NexusLlmHost::OnCallDeadlineTimeout,
                                weak_factory_.GetWeakPtr(), request_id));
    pending.deadline_timer = std::move(timer);
  }
  pending_calls_.emplace(request_id, std::move(pending));

  provider->DoGenerate(
      call_options,
      base::BindOnce(&NexusLlmHost::OnGenerateComplete,
                     weak_factory_.GetWeakPtr(), request_id));
}

void NexusLlmHost::StreamStart(mojom::NexusLlmRequestParamsPtr request,
                               StreamStartCallback callback) {
  if (!request) {
    std::move(callback).Run(false, std::nullopt,
                            std::make_optional("Missing LLM request"));
    return;
  }

  std::string provider_id;
  LlmProvider* provider = ResolveProvider(profile_, *request, &provider_id);
  if (!provider) {
    std::move(callback).Run(
        false, std::nullopt,
        std::make_optional(
            "No Nexus LLM provider is available for the requested stream"));
    return;
  }
  if (!provider->IsConfigured()) {
    std::move(callback).Run(
        false, std::nullopt,
        std::make_optional("Requested Nexus LLM provider is not configured"));
    return;
  }

  EnsureProviderAuthThen(
      provider_id,
      base::BindOnce(&NexusLlmHost::ContinueStreamStart,
                     weak_factory_.GetWeakPtr(), std::move(request),
                     std::move(callback)));
}

void NexusLlmHost::ContinueStreamStart(mojom::NexusLlmRequestParamsPtr request,
                                       StreamStartCallback callback,
                                       bool auth_ok) {
  if (!auth_ok) {
    std::move(callback).Run(
        false, std::nullopt,
        std::make_optional(
            "Sign in to this provider in Nexus settings to continue"));
    return;
  }

  std::string provider_id;
  LlmProvider* provider = ResolveProvider(profile_, *request, &provider_id);
  if (!provider || !provider->IsConfigured()) {
    std::move(callback).Run(
        false, std::nullopt,
        std::make_optional("Requested Nexus LLM provider is not configured"));
    return;
  }

  const std::string stream_id =
      base::Uuid::GenerateRandomV4().AsLowercaseString();
  const std::string model_id =
      ResolveModelId(profile_, *request, provider_id, provider);

  StreamState state;
  state.stream_id = stream_id;
  state.provider_id = provider_id;
  state.model_id = model_id;
  QueueMessageStartEvent(&state);
  streams_.emplace(stream_id, std::move(state));

  const base::DictValue call_options =
      BuildCallOptions(profile_, *request, provider_id, provider, stream_id);

  provider->DoStream(
      call_options,
      base::BindRepeating(&NexusLlmHost::OnStreamChunk,
                          weak_factory_.GetWeakPtr(), stream_id),
      base::BindOnce(&NexusLlmHost::OnStreamDone,
                     weak_factory_.GetWeakPtr(), stream_id));

  std::move(callback).Run(true, std::make_optional(stream_id), std::nullopt);
}

void NexusLlmHost::StreamRead(const std::string& stream_id,
                              std::optional<double> deadline_ms,
                              std::optional<double> wait_ms,
                              base::DictValue timeline,
                              StreamReadCallback callback) {
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) {
    std::move(callback).Run(false, nullptr,
                            std::make_optional("Unknown Nexus LLM stream id"));
    return;
  }

  StreamState& state = it->second;
  timeline.Set("t_host_read", NowEpochMs());

  // Legacy fast path: data already queued, or stream is done — resolve
  // immediately with whatever is buffered (possibly empty on done).
  const bool has_data = !state.queued_events.empty();
  if (has_data || state.done) {
    auto result = mojom::NexusLlmStreamReadResult::New();
    result->events = std::move(state.queued_events);
    result->done = state.done;
    state.queued_events.clear();
    result->status = state.done ? "done" : "ok";
    timeline.Set("t_resolve", NowEpochMs());
    result->timeline = std::move(timeline);

    if (state.done) {
      streams_.erase(it);
    }

    std::move(callback).Run(true, std::move(result), std::nullopt);
    return;
  }

  // No data, not done, and the caller asked us to wait — HOLD the read.
  if (deadline_ms.has_value() || wait_ms.has_value()) {
    // Compute the hold window: min(wait_ms, deadline_ms - now). Whichever
    // fires first wins; both are optional, and either alone is sufficient.
    const double now_ms = NowEpochMs();
    double hold_ms = -1;
    if (wait_ms.has_value() && *wait_ms > 0) {
      hold_ms = *wait_ms;
    }
    if (deadline_ms.has_value()) {
      const double remaining_ms = *deadline_ms - now_ms;
      if (hold_ms < 0 || remaining_ms < hold_ms) {
        hold_ms = remaining_ms;
      }
    }

    if (hold_ms > 0) {
      // Disallow stacking: a second streamRead for the same stream while one
      // is held replaces the previous pending read. The previous read is
      // RESOLVED (empty, status "ok") rather than dropped — destroying an
      // unrun mojo response callback while the receiver is still bound
      // triggers a DCHECK/bad-message, and the guest contract only ever has
      // one outstanding read per stream anyway.
      ResolveHeldRead(state, "ok");
      PendingStreamRead pending;
      pending.callback = std::move(callback);
      pending.timeline = std::move(timeline);
      auto timer = std::make_unique<base::OneShotTimer>();
      timer->Start(
          FROM_HERE, base::Milliseconds(static_cast<int64_t>(hold_ms)),
          base::BindOnce(&NexusLlmHost::OnStreamReadHoldTimeout,
                         weak_factory_.GetWeakPtr(), stream_id));
      pending.hold_timer = std::move(timer);
      state.pending_read = std::move(pending);
      return;
    }
    // hold_ms <= 0: deadline already passed — fall through to immediate
    // empty+timeout resolution below.
  }

  // No hold requested (or deadline already passed): legacy immediate
  // resolution with an empty queue. `status:'timeout'` when a deadline was
  // supplied and has already elapsed; otherwise 'ok'.
  auto result = mojom::NexusLlmStreamReadResult::New();
  result->done = false;
  LlmBroker* broker = GetBroker(profile_);
  // Queue time is not provider silence — see OnStreamReadHoldTimeout. A
  // request still in the provider's FIFO reports `queued` and keeps both its
  // FIFO position and its host-side StreamState.
  const bool still_queued = broker && broker->IsRequestQueued(stream_id);
  const bool deadline_elapsed =
      !still_queued && deadline_ms.has_value() && NowEpochMs() >= *deadline_ms;
  result->status =
      still_queued ? "queued" : (deadline_elapsed ? "timeout" : "ok");
  timeline.Set("t_resolve", NowEpochMs());
  result->timeline = std::move(timeline);

  // The guest abandons a stream on ANY `status:'timeout'` read (it throws
  // and starts a fresh stream). Mirror OnStreamReadHoldTimeout: tear down
  // the upstream loader and drop the entry, or both leak per abandoned
  // stream.
  if (deadline_elapsed) {
    if (broker) {
      broker->CancelRequest(stream_id);
    }
    streams_.erase(it);
  }

  std::move(callback).Run(true, std::move(result), std::nullopt);
}

bool NexusLlmHost::ResolveHeldRead(StreamState& state, const char* status) {
  if (!state.pending_read.has_value()) {
    return false;
  }

  PendingStreamRead pending = std::move(*state.pending_read);
  state.pending_read.reset();
  if (pending.hold_timer) {
    pending.hold_timer->Stop();
  }

  auto result = mojom::NexusLlmStreamReadResult::New();
  result->events = std::move(state.queued_events);
  result->done = state.done;
  state.queued_events.clear();
  result->status = status;
  pending.timeline.Set("t_resolve", NowEpochMs());
  result->timeline = std::move(pending.timeline);
  // POST the callback instead of running it synchronously. The callback is
  // the bridge adapter's read lambda, which can decide to call
  // NexusLlmHost::Cancel — synchronously that would re-enter this host,
  // erase the StreamState most callers of ResolveHeldRead still reference
  // on their stack (OnStreamChunk's `state`, OnStreamDone/Cancel's `it`),
  // and re-enter the provider from inside its own chunk-emission chain —
  // the exact re-entrancy family behind the memcmp field crash. Posting
  // breaks the loop; same-sequence posting preserves per-stream ordering.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(std::move(pending.callback), true, std::move(result),
                     std::optional<std::string>()));
  return true;
}

void NexusLlmHost::OnStreamReadHoldTimeout(const std::string& stream_id) {
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) {
    return;
  }

  StreamState& state = it->second;
  if (!state.pending_read.has_value()) {
    return;
  }

  // The hold window expired with no new data — resolve with status:'timeout'
  // (empty chunks, done=false) AND cancel the upstream loader so the socket
  // closes server-side. The JS guest has its own 100s transport fallback;
  // the native side is the PRIMARY timeout.
  PendingStreamRead pending = std::move(*state.pending_read);
  state.pending_read.reset();
  if (pending.hold_timer) {
    pending.hold_timer->Stop();
  }

  LlmBroker* broker = GetBroker(profile_);

  // ...UNLESS the request never left the building. Under a provider
  // concurrency cap (chatgpt-pro's max_concurrent_streams: 1) the request can
  // still be sitting in the FIFO with no socket open, so the silence is OUR
  // queue, not the provider. Failing the read here made the guest throw,
  // cancel, and retry — which re-enqueued it at the BACK of the same FIFO, so
  // a background stream could burn its whole retry ladder behind one long
  // foreground turn and never reach the provider at all.
  //
  // Resolve `status:'queued'` instead: no cancel (the entry keeps its FIFO
  // position) and no erase (the guest re-issues a read against the same
  // stream id). Guests that predate the value see an unknown non-'timeout'
  // status, treat the host as hold-capable, and re-issue the read too — the
  // same behaviour, one 90s window at a time.
  if (broker && broker->IsRequestQueued(stream_id)) {
    auto queued_result = mojom::NexusLlmStreamReadResult::New();
    queued_result->done = false;
    queued_result->status = "queued";
    pending.timeline.Set("t_resolve", NowEpochMs());
    queued_result->timeline = std::move(pending.timeline);
    std::move(pending.callback).Run(true, std::move(queued_result),
                                    std::nullopt);
    return;
  }

  if (broker) {
    broker->CancelRequest(stream_id);
  }

  auto result = mojom::NexusLlmStreamReadResult::New();
  result->done = false;
  result->status = "timeout";
  pending.timeline.Set("t_resolve", NowEpochMs());
  result->timeline = std::move(pending.timeline);
  std::move(pending.callback).Run(true, std::move(result), std::nullopt);

  // The guest abandons this stream on a native timeout (it throws and starts a
  // fresh stream under a new id) and sends neither another read nor a cancel,
  // so drop the entry now — otherwise it leaks in `streams_` for the host's
  // lifetime. The upstream loader was already cancelled above.
  streams_.erase(it);
}

void NexusLlmHost::Cancel(const std::string& stream_id,
                          CancelCallback callback) {
  if (LlmBroker* broker = GetBroker(profile_)) {
    broker->CancelRequest(stream_id);
  }
  auto it = streams_.find(stream_id);
  if (it != streams_.end()) {
    // Drain any held streamRead so its callback resolves before the stream
    // state is erased — otherwise the guest's outstanding read would hang
    // until its own transport-level fallback fires. Mark the state done
    // first so the resolved read carries a self-consistent
    // {done: true, status: "done"} (and the bridge adapter drops its
    // per-stream state via the stream_done signal).
    it->second.done = true;
    ResolveHeldRead(it->second, "done");
    streams_.erase(it);
  }
  std::move(callback).Run(true, std::nullopt);
}

void NexusLlmHost::OnGenerateComplete(const std::string& request_id,
                                      GenerateResult result,
                                      std::optional<std::string> error) {
  auto it = pending_calls_.find(request_id);
  if (it == pending_calls_.end()) {
    // The deadline already fired and consumed the pending call (or the host
    // is mid-teardown). Nothing to resolve.
    return;
  }

  PendingCall pending = std::move(it->second);
  pending_calls_.erase(it);
  if (pending.deadline_timer) {
    pending.deadline_timer->Stop();
  }

  if (error.has_value()) {
    std::move(pending.callback).Run(false, nullptr, std::move(error));
    return;
  }

  pending.timeline.Set("t_resolve", NowEpochMs());
  auto response = mojom::NexusLlmCallResult::New();
  response->message = BuildAssistantMessage(pending.resolved_model_id, result);
  response->status = "ok";
  response->timeline = std::move(pending.timeline);
  std::move(pending.callback).Run(true, std::move(response), std::nullopt);
}

void NexusLlmHost::OnCallDeadlineTimeout(const std::string& request_id) {
  auto it = pending_calls_.find(request_id);
  if (it == pending_calls_.end()) {
    return;
  }

  PendingCall pending = std::move(it->second);
  pending_calls_.erase(it);

  // Same queue-vs-silence distinction as OnStreamReadHoldTimeout: a blocking
  // call that is still in the provider's FIFO never opened a socket, so the
  // deadline measured OUR queue rather than the provider. Unlike the stream
  // path this DOES cancel — the guest re-issues as a brand-new request id, so
  // a surviving queue entry would later start an orphan whose completion
  // callback has nowhere to go.
  //
  // Deliberately resolved as an ERROR, not `status:'queued'`: a guest that
  // predates the queued verdict only special-cases `'timeout'` — on any other
  // status it would read `result.message`, which is the default-constructed
  // EMPTY assistant message below, and render a silent empty turn (the
  // "silent stop" bug class). An error string degrades safely on every guest
  // vintage: all of them throw it into the provider-retry classifier. The
  // wording is classifier-facing — "timed out"/"upstream" are the keywords
  // `classifyProviderConnectivityIssue` (nexus_web providerConnectivity.ts)
  // maps to a RETRYABLE verdict, so the retry ladder (with backoff) owns the
  // re-issue on both old and new guests.
  LlmBroker* broker = GetBroker(profile_);
  const bool was_queued = broker && broker->IsRequestQueued(request_id);

  // Cancel the upstream loader so the provider socket closes server-side.
  if (broker) {
    broker->CancelRequest(request_id);
  }

  if (was_queued) {
    std::move(pending.callback)
        .Run(false, nullptr,
             std::make_optional<std::string>(
                 "queue_timeout: request timed out waiting for a provider "
                 "concurrency slot and was cancelled before dispatch (the "
                 "upstream provider never received it)"));
    return;
  }

  pending.timeline.Set("t_resolve", NowEpochMs());
  auto response = mojom::NexusLlmCallResult::New();
  // Default-construct the (non-nullable) message so the result stays valid if
  // it ever crosses a real mojo serialization boundary — today the host→
  // adapter hop is same-process so a null message is tolerated, but a remote
  // pipe would CHECK-fail validation on the unset non-nullable struct field.
  response->message = mojom::NexusLlmAssistantMessage::New();
  response->status = "timeout";
  response->timeline = std::move(pending.timeline);
  std::move(pending.callback).Run(true, std::move(response), std::nullopt);
}

void NexusLlmHost::OnStreamChunk(const std::string& stream_id,
                                 base::DictValue chunk) {
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) {
    return;
  }

  StreamState& state = it->second;
  const std::string* type = chunk.FindString("type");
  if (!type) {
    return;
  }

  // Backpressure: if the guest has stopped reading (wedged tab, JS error
  // after streamStart) the queue would otherwise grow without bound while a
  // provider keeps streaming — the idle guard never fires because bytes keep
  // arriving. Past the cap, kill the upstream request and terminate the
  // stream with an error event; a late read still drains the buffered error.
  constexpr size_t kMaxQueuedStreamEvents = 8192;
  if (state.queued_events.size() >= kMaxQueuedStreamEvents &&
      !state.terminal_event_queued) {
    if (LlmBroker* broker = GetBroker(profile_)) {
      broker->CancelRequest(stream_id);
    }
    QueueErrorEvent(&state,
                    "Nexus LLM stream buffered too many unread events; "
                    "cancelled upstream request");
    state.done = true;
    return;
  }

  // Resolve the held streamRead (if any) whenever this chunk produces a
  // queued event. `wake` is called at each branch's tail; branches that
  // early-return without queueing anything skip it.
  auto wake = [this, &state]() { ResolveHeldRead(state, "ok"); };

  if (*type == "text-delta") {
    const std::string* text_delta = chunk.FindString("textDelta");
    if (!text_delta || text_delta->empty()) {
      return;
    }

    auto event = mojom::NexusLlmStreamEvent::New();
    event->type = mojom::NexusLlmStreamEventType::kTextDelta;
    event->text_delta = *text_delta;
    EnqueueEvent(&state, std::move(event));
    wake();
    return;
  }

  if (*type == "reasoning-delta" || *type == "reasoning") {
    const std::string* text_delta = chunk.FindString("textDelta");
    if (!text_delta) {
      text_delta = chunk.FindString("text");
    }
    if (!text_delta || text_delta->empty()) {
      return;
    }

    auto event = mojom::NexusLlmStreamEvent::New();
    event->type = mojom::NexusLlmStreamEventType::kReasoningDelta;
    event->text_delta = *text_delta;
    EnqueueEvent(&state, std::move(event));
    wake();
    return;
  }

  if (*type == "tool-call-streaming-start") {
    // Treat an EMPTY id like a missing one (matching the "tool-call" branch)
    // — an empty string would otherwise become a valid correlation key and
    // collide across distinct id-less calls.
    PendingToolCall pending;
    const std::string* start_id = chunk.FindString("toolCallId");
    pending.tool_call_id =
        (start_id && !start_id->empty())
            ? *start_id
            : base::Uuid::GenerateRandomV4().AsLowercaseString();
    if (const auto* tool_name = chunk.FindString("toolName");
        tool_name && !tool_name->empty()) {
      pending.tool_name = *tool_name;
    }
    if (state.pending_tool_calls.find(pending.tool_call_id) ==
        state.pending_tool_calls.end()) {
      state.pending_tool_call_order.push_back(pending.tool_call_id);
    }
    state.pending_tool_calls[pending.tool_call_id] = pending;

    auto event = mojom::NexusLlmStreamEvent::New();
    event->type = mojom::NexusLlmStreamEventType::kToolCallStart;
    event->tool_use_id = pending.tool_call_id;
    if (!pending.tool_name.empty()) {
      event->name = pending.tool_name;
    }
    EnqueueEvent(&state, std::move(event));
    wake();
    return;
  }

  if (*type == "tool-call-delta") {
    const std::string* delta_id = chunk.FindString("toolCallId");
    std::string tool_call_id =
        (delta_id && !delta_id->empty())
            ? *delta_id
            : base::Uuid::GenerateRandomV4().AsLowercaseString();

    PendingToolCall& pending = state.pending_tool_calls[tool_call_id];
    if (pending.tool_call_id.empty()) {
      pending.tool_call_id = tool_call_id;
      state.pending_tool_call_order.push_back(tool_call_id);
    }
    if (const auto* tool_name = chunk.FindString("toolName");
        tool_name && !tool_name->empty()) {
      pending.tool_name = *tool_name;
    }
    if (const auto* args_delta = chunk.FindString("argsTextDelta");
        args_delta && !args_delta->empty()) {
      pending.args_json.append(*args_delta);
    }

    auto event = mojom::NexusLlmStreamEvent::New();
    event->type = mojom::NexusLlmStreamEventType::kToolCallDelta;
    event->tool_use_id = tool_call_id;
    if (!pending.tool_name.empty()) {
      event->name = pending.tool_name;
    }
    if (const auto* args_delta = chunk.FindString("argsTextDelta");
        args_delta && !args_delta->empty()) {
      event->args_text_delta = *args_delta;
    }
    EnqueueEvent(&state, std::move(event));
    wake();
    return;
  }

  if (*type == "tool-call") {
    std::string tool_call_id;
    if (const auto* id = chunk.FindString("toolCallId"); id && !id->empty()) {
      tool_call_id = *id;
    }

    std::string tool_name;
    if (const auto* name = chunk.FindString("toolName");
        name && !name->empty()) {
      tool_name = *name;
    }

    std::string args_json;
    if (const auto* args = chunk.FindString("args"); args && !args->empty()) {
      args_json = *args;
    }

    if (!tool_call_id.empty()) {
      auto pending_it = state.pending_tool_calls.find(tool_call_id);
      if (pending_it != state.pending_tool_calls.end()) {
        if (args_json.empty()) {
          args_json = pending_it->second.args_json;
        }
        if (tool_name.empty()) {
          tool_name = pending_it->second.tool_name;
        }
        state.pending_tool_calls.erase(pending_it);
      }
      state.pending_tool_call_order.erase(
          std::remove(state.pending_tool_call_order.begin(),
                      state.pending_tool_call_order.end(), tool_call_id),
          state.pending_tool_call_order.end());
    }

    QueueCompletedToolCallEvent(&state, tool_call_id, tool_name, args_json);
    wake();
    return;
  }

  if (*type == "usage-update") {
    QueueUsageUpdate(&state, UsageFromProviderDict(chunk.FindDict("usage")));
    wake();
    return;
  }

  if (*type == "finish") {
    FlushPendingToolCalls(&state);
    FinishReason reason = FinishReason::kUnknown;
    if (const auto* finish_reason = chunk.FindString("finishReason");
        finish_reason && !finish_reason->empty()) {
      reason = ParseFinishReasonString(*finish_reason);
    }
    QueueFinishEvent(&state, reason, chunk.FindDict("usage"));
    wake();
    return;
  }

  if (*type == "error") {
    std::string message = "Unknown Nexus LLM stream error";
    if (const base::DictValue* error = chunk.FindDict("error")) {
      if (const auto* error_message = error->FindString("message");
          error_message && !error_message->empty()) {
        message = *error_message;
      }
    } else if (const auto* error_message = chunk.FindString("error");
               error_message && !error_message->empty()) {
      message = *error_message;
    }
    QueueErrorEvent(&state, message);
    state.done = true;
    // Resolve a held read with the terminal state ("done", matching
    // OnStreamDone). If the held read consumed it, the guest has observed
    // `done` and will issue no further read — erase the entry now, or it
    // leaks: the later OnStreamDone finds no held read and leaves the entry
    // for a next StreamRead that never comes.
    if (ResolveHeldRead(state, "done")) {
      streams_.erase(it);
    }
  }
}

void NexusLlmHost::OnStreamDone(const std::string& stream_id,
                                std::optional<std::string> error) {
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) {
    return;
  }

  if (error.has_value()) {
    QueueErrorEvent(&it->second, *error);
  } else if (!it->second.terminal_event_queued) {
    FlushPendingToolCalls(&it->second);
    QueueFinishEvent(&it->second, FinishReason::kUnknown, nullptr);
  }

  it->second.done = true;
  // Wake the held streamRead (if any) — the terminal event is now queued and
  // the stream is done, so the guest's outstanding read can resolve. When a
  // held read consumed the terminal state, the guest has received `done` and
  // will issue no further read (it sends no cancel on normal completion), so
  // drop the entry here — nothing else erases it on this path, and leaving it
  // would leak one StreamState per completed stream over the host's lifetime.
  // A legacy polling guest has no held read: ResolveHeldRead returns false and
  // its next StreamRead observes `done` and erases the entry itself (unchanged).
  if (ResolveHeldRead(it->second, "done")) {
    streams_.erase(it);
  }
}

// --- Public helpers re-exported from the anonymous namespace ----------------

bool IsNexusProviderVerified(PrefService* prefs,
                             const std::string& provider_id) {
  return IsProviderVerified(prefs, provider_id);
}

bool IsNexusModelEnabled(PrefService* prefs,
                         const std::string& provider_id,
                         const std::string& model_id) {
  return IsModelEnabled(prefs, provider_id, model_id);
}

std::string GetNexusProviderIconSvg(const std::string& provider_id) {
  return ResolveProviderIconSvg(provider_id);
}

}  // namespace nexus
