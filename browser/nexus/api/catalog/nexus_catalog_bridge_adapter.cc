#include "custom_browser/browser/nexus/api/catalog/nexus_catalog_bridge_adapter.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/task/thread_pool.h"
#include "base/values.h"
#include "build/build_config.h"
#include "custom_browser/browser/nexus/api/catalog/prompt_section_renderer.h"
#include "custom_browser/browser/nexus/api/shell/host_shell.h"
#include "custom_browser/browser/nexus/tools/nexus_skills_registry.gen.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_schema.h"
#include "custom_browser/browser/nexus/tools/tool_catalog.h"

namespace nexus {

NexusCatalogBridgeAdapter::NexusCatalogBridgeAdapter(
    NexusBridgeResponseDelegate* delegate)
    : ::nexus::generated::CatalogApiBase(delegate) {
  // Register(...) calls live in `CatalogApiBase`'s ctor, generated
  // from api/declarations/catalog/*.api.json. Adding a new catalog
  // wire action: drop a *.api.json under api/declarations/catalog/,
  // add a Handle*OnUI() override here. The codegen wires the
  // dispatch automatically.
}

NexusCatalogBridgeAdapter::~NexusCatalogBridgeAdapter() = default;

void NexusCatalogBridgeAdapter::HandleListHostedToolsOnUI(
    ::nexus::generated::ListHostedToolsInput /*input*/,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  // The Bash / PowerShell tools are gated by which shells the host can run
  // (git-bash on Windows; pwsh/powershell). Resolving that needs PATH stats
  // — blocking I/O, which the header contract forbids on the UI thread — so
  // resolve off-thread, then filter the catalog and reply on UI.
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&ResolveHostShellAvailability),
      base::BindOnce(&NexusCatalogBridgeAdapter::ReplyWithFilteredCatalog,
                     weak_ptr_factory_.GetWeakPtr(), std::move(done)));
}

void NexusCatalogBridgeAdapter::ReplyWithFilteredCatalog(
    ::nexus::BridgeResponseCallback done,
    HostShellAvailability availability) {
  // Gate the two shell tools by platform + host capability; everything else
  // passes through. Wire format unchanged (`BuildHostedToolsList`).
  //   - Bash: always on POSIX; on Windows only when git-bash is on PATH
  //     (otherwise the model gets PowerShell instead).
  //   - PowerShell: Windows only, and only when pwsh/powershell resolves.
  std::vector<ToolSchema> filtered;
  for (const ToolSchema& schema : ToolCatalog::Get().schemas()) {
    if (schema.name == "Bash") {
#if BUILDFLAG(IS_WIN)
      if (!availability.bash) {
        continue;
      }
#endif
    } else if (schema.name == "PowerShell") {
#if BUILDFLAG(IS_WIN)
      if (!availability.powershell()) {
        continue;
      }
#else
      continue;
#endif
    }
    filtered.push_back(schema);
  }

  base::ListValue tools = BuildHostedToolsList(filtered);
  base::DictValue result;
  result.Set("hostedTools", std::move(tools));
  std::move(done).Success(std::move(result));
}

void NexusCatalogBridgeAdapter::HandleRenderPromptSectionOnUI(
    ::nexus::generated::RenderPromptSectionInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  // Unknown section names fall through to an empty string per the
  // renderer's contract — matches the TS behavior where an
  // unsupported placeholder leaves the slot empty.
  std::string text = RenderPromptSection(input.name, input.enabledToolNames);

  base::DictValue result;
  result.Set("text", std::move(text));
  std::move(done).Success(std::move(result));
}

void NexusCatalogBridgeAdapter::HandleListPromptSectionsOnUI(
    ::nexus::generated::ListPromptSectionsInput /*input*/,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  base::ListValue sections;
  for (const std::string& name : ListPromptSectionNames()) {
    sections.Append(name);
  }

  base::DictValue result;
  result.Set("sections", std::move(sections));
  std::move(done).Success(std::move(result));
}

void NexusCatalogBridgeAdapter::HandleListSkillsOnUI(
    ::nexus::generated::ListSkillsInput /*input*/,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    ::nexus::BridgeResponseCallback done) {
  // Serve the skill catalog. Each skill's `toolNames` is derived inside
  // BuildHostedSkillsList by intersecting the live tool catalog with the
  // skill (per-tool `skill` field) — single source of truth, no drift.
  base::ListValue skills = BuildHostedSkillsList(
      ::nexus::generated::AllSkills(), ToolCatalog::Get().schemas());

  base::DictValue result;
  result.Set("hostedSkills", std::move(skills));
  std::move(done).Success(std::move(result));
}

}  // namespace nexus
