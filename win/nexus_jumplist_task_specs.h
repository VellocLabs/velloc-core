#ifndef CUSTOM_BROWSER_WIN_NEXUS_JUMPLIST_TASK_SPECS_H_
#define CUSTOM_BROWSER_WIN_NEXUS_JUMPLIST_TASK_SPECS_H_

#include <vector>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "components/policy/core/common/policy_pref_names.h"

namespace custom_browser {

// One product taskbar Jump List task: its kind (selects the title + icon in
// the decorator, nexus_jumplist_tasks.cc) and the relaunch command line. This
// type is the shared vocabulary between the pure policy core
// (BuildNexusJumpListTaskSpecs) and the decorator.
//
// The Nexus product currently emits NO tasks (see BuildNexusJumpListTaskSpecs):
// there is deliberately no "New <Product> window" task (the Nexus window is a
// per-profile singleton, already the default target of a plain taskbar-icon
// click / cold launch), and the "New browser window" / "New Incognito Window"
// escape-hatches were intentionally removed from the taskbar menu as well.
struct NexusJumpListTaskSpec {
  enum class Kind { kBrowserWindow, kIncognitoWindow };

  Kind kind;
  base::CommandLine command_line{base::CommandLine::NO_PROGRAM};
};

// Pure, string-free core (unit-testable without a ResourceBundle): returns the
// product's taskbar Jump List task specs in display order. The Nexus product
// deliberately exposes no tasks, so this currently returns an empty list
// regardless of the incognito policy (see the .cc for the rationale).
// Worker-thread safe: no Browser*/Profile/UI access.
std::vector<NexusJumpListTaskSpec> BuildNexusJumpListTaskSpecs(
    policy::IncognitoModeAvailability incognito_availability,
    const base::FilePath& cmd_line_profile_dir);

}  // namespace custom_browser

#endif  // CUSTOM_BROWSER_WIN_NEXUS_JUMPLIST_TASK_SPECS_H_
