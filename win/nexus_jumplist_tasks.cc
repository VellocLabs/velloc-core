#include "custom_browser/win/nexus_jumplist_tasks.h"

#include <string_view>
#include <utility>

#include "base/base_paths.h"
#include "base/memory/scoped_refptr.h"
#include "base/path_service.h"
#include "base/strings/string_util.h"
#include "chrome/common/chrome_icon_resources_win.h"
#include "chrome/grit/generated_resources.h"
#include "chrome/install_static/install_util.h"
#include "custom_browser/resources/grit/custom_browser_strings.h"
#include "ui/base/l10n/l10n_util.h"

namespace custom_browser {

namespace {

// Strips '&' accelerators so the title can be shared safely (mirrors
// jumplist.cc's UpdateTaskCategory).
std::u16string CleanTitle(std::u16string title) {
  base::ReplaceSubstringsAfterOffset(&title, 0, u"&", std::u16string_view());
  return title;
}

}  // namespace

bool BuildNexusJumpListTasks(
    policy::IncognitoModeAvailability incognito_availability,
    const base::FilePath& cmd_line_profile_dir,
    ShellLinkItemList* out) {
  base::FilePath chrome_path;
  if (!base::PathService::Get(base::FILE_EXE, &chrome_path)) {
    return false;
  }
  const int app_icon_index = install_static::GetAppIconResourceIndex();

  for (NexusJumpListTaskSpec& spec : BuildNexusJumpListTaskSpecs(
           incognito_availability, cmd_line_profile_dir)) {
    auto link = base::MakeRefCounted<ShellLinkItem>();
    // Transfer the spec's switches (user-data-dir / profile-dir / task switch)
    // into the link's command line.
    link->GetCommandLine()->AppendArguments(spec.command_line,
                                            /*include_program=*/false);
    switch (spec.kind) {
      case NexusJumpListTaskSpec::Kind::kBrowserWindow:
        link->set_title(CleanTitle(
            l10n_util::GetStringUTF16(IDS_NEXUS_JUMPLIST_NEW_BROWSER_WINDOW)));
        link->set_icon(chrome_path, app_icon_index);
        break;
      case NexusJumpListTaskSpec::Kind::kIncognitoWindow:
        link->set_title(
            CleanTitle(l10n_util::GetStringUTF16(IDS_NEW_INCOGNITO_WINDOW)));
        link->set_icon(chrome_path, icon_resources::kIncognitoIndex);
        break;
    }
    out->push_back(std::move(link));
  }

  return true;
}

}  // namespace custom_browser
