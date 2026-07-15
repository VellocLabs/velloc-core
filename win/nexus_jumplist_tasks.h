#ifndef CUSTOM_BROWSER_WIN_NEXUS_JUMPLIST_TASKS_H_
#define CUSTOM_BROWSER_WIN_NEXUS_JUMPLIST_TASKS_H_

#include "base/files/file_path.h"
#include "chrome/browser/win/jumplist_updater.h"
#include "custom_browser/win/nexus_jumplist_task_specs.h"

namespace custom_browser {

// Decorates the product Jump List task specs (see BuildNexusJumpListTaskSpecs)
// with localized titles + icons and appends them to |out|. Returns true so the
// caller skips the default New Window / New Incognito Window tasks. The Nexus
// product currently emits no specs, so |out| is left empty and the taskbar
// Tasks category ends up empty. The Jump List is product/app-id scoped (one per
// profile), so these tasks apply to all of the product's windows, not
// per-window. Worker-thread safe.
bool BuildNexusJumpListTasks(
    policy::IncognitoModeAvailability incognito_availability,
    const base::FilePath& cmd_line_profile_dir,
    ShellLinkItemList* out);

}  // namespace custom_browser

#endif  // CUSTOM_BROWSER_WIN_NEXUS_JUMPLIST_TASKS_H_
