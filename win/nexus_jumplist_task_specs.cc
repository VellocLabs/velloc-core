#include "custom_browser/win/nexus_jumplist_task_specs.h"

namespace custom_browser {

std::vector<NexusJumpListTaskSpec> BuildNexusJumpListTaskSpecs(
    policy::IncognitoModeAvailability /*incognito_availability*/,
    const base::FilePath& /*cmd_line_profile_dir*/) {
  // The Nexus product deliberately exposes NO taskbar Jump List "Tasks". The
  // Nexus window is the per-profile singleton that a plain taskbar-icon click /
  // cold launch already opens, and the "New browser window" (--browser-mode)
  // and "New Incognito Window" escape-hatches were intentionally dropped from
  // the taskbar menu. Returning an empty list makes UpdateTaskCategory add an
  // empty Tasks category and skip the upstream default New Window / New
  // Incognito Window items. The incognito policy no longer gates anything now
  // that no tasks are emitted.
  return {};
}

}  // namespace custom_browser
