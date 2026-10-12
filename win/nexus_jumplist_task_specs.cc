#include "custom_browser/win/nexus_jumplist_task_specs.h"

namespace custom_browser {

std::vector<NexusJumpListTaskSpec> BuildNexusJumpListTaskSpecs(
    policy::IncognitoModeAvailability /*incognito_availability*/,
    const base::FilePath& /*cmd_line_profile_dir*/) {
  // Deliberately no Jump List tasks: the Nexus window is what a plain taskbar
  // click already opens. An empty list also skips upstream's default items.
  return {};
}

}  // namespace custom_browser
