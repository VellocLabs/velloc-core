#ifndef CUSTOM_BROWSER_WIN_NEXUS_JUMPLIST_TASK_SPECS_H_
#define CUSTOM_BROWSER_WIN_NEXUS_JUMPLIST_TASK_SPECS_H_

#include <vector>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "components/policy/core/common/policy_pref_names.h"

namespace custom_browser {

// One product taskbar Jump List task: its kind (selects the title + icon in
// the decorator) and the relaunch command line (current --user-data-dir +
// launch profile dir + the task-specific switch).
//
// There is deliberately no "New <Product> window" task: the Nexus window is a
// per-profile singleton and is the default target of a plain taskbar-icon
// click / cold launch, so the Jump List offers only the browser escape-hatches.
// (A no-switch relaunch would also warm-misroute: a second instance forwarded
// to the running process finds the kNexus startup-default already consumed and
// opens a normal window, not Nexus.)
struct NexusJumpListTaskSpec {
  enum class Kind { kBrowserWindow, kIncognitoWindow };

  Kind kind;
  base::CommandLine command_line{base::CommandLine::NO_PROGRAM};
};

// Pure, string-free core (unit-testable without a ResourceBundle): returns the
// product Jump List task specs in display order, honoring the incognito policy
// — kForced drops the browser-window task (mirrors upstream dropping its
// non-incognito "New Window"); kDisabled drops incognito.
// Worker-thread safe: no Browser*/Profile/UI access.
std::vector<NexusJumpListTaskSpec> BuildNexusJumpListTaskSpecs(
    policy::IncognitoModeAvailability incognito_availability,
    const base::FilePath& cmd_line_profile_dir);

}  // namespace custom_browser

#endif  // CUSTOM_BROWSER_WIN_NEXUS_JUMPLIST_TASK_SPECS_H_
