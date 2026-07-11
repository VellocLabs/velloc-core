#include "custom_browser/browser/nexus/api/catalog/nexus_catalog_bridge_adapter.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/strings/string_split.h"
#include "base/task/thread_pool.h"
#include "base/values.h"
#include "build/build_config.h"
#include "custom_browser/browser/nexus/api/catalog/prompt_section_renderer.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_schema.h"
#include "custom_browser/browser/nexus/tools/tool_catalog.h"

namespace nexus {

namespace {

#if BUILDFLAG(IS_WIN)
constexpr char kPathSeparator = ';';
const char* const kExecutableExtensions[] = {".exe", ".com", ".cmd",
                                             ".bat", ".ps1", ""};
#else
constexpr char kPathSeparator = ':';
const char* const kExecutableExtensions[] = {""};
#endif

// Synchronous PATH lookup — no spawn, no `--version` probe. MUST run on a
// MayBlock thread (it stats files). Mirrors the PATH-walk in
// ShellResolver::FindOnPath (utility process) but browser-side and probe-
// free, just to answer "is this shell installed?".
bool IsExecutableOnPath(const std::string& tool_id) {
  std::unique_ptr<base::Environment> env = base::Environment::Create();
  std::optional<std::string> path_value = env->GetVar("PATH");
  if (!path_value.has_value() || path_value->empty()) {
    return false;
  }
  for (std::string_view part : base::SplitStringPiece(
           *path_value, std::string(1, kPathSeparator), base::TRIM_WHITESPACE,
           base::SPLIT_WANT_NONEMPTY)) {
    base::FilePath dir = base::FilePath::FromUTF8Unsafe(part);
    for (const char* ext : kExecutableExtensions) {
      base::FilePath candidate = dir.AppendASCII(tool_id + std::string(ext));
      if (base::PathExists(candidate) && !base::DirectoryExists(candidate)) {
        return true;
      }
    }
  }
  return false;
}

// Off-thread resolution of which shells the host can run. `bash` covers
// git-bash on Windows; `pwsh` OR Windows `powershell` counts as
// PowerShell-capable.
CatalogShellAvailability ResolveShellAvailability() {
  CatalogShellAvailability availability;
  availability.bash = IsExecutableOnPath("bash");
  availability.pwsh =
      IsExecutableOnPath("pwsh") || IsExecutableOnPath("powershell");
  return availability;
}

}  // namespace

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
      base::BindOnce(&ResolveShellAvailability),
      base::BindOnce(&NexusCatalogBridgeAdapter::ReplyWithFilteredCatalog,
                     weak_ptr_factory_.GetWeakPtr(), std::move(done)));
}

void NexusCatalogBridgeAdapter::ReplyWithFilteredCatalog(
    ::nexus::BridgeResponseCallback done,
    CatalogShellAvailability availability) {
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
      if (!availability.pwsh) {
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

}  // namespace nexus
