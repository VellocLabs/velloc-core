#include "custom_browser/win/nexus_jumplist_task_specs.h"

#include <utility>

#include "chrome/common/chrome_switches.h"
#include "custom_browser/common/custom_browser_switches.h"

namespace custom_browser {

namespace {

// Builds the relaunch command line shared by every task: the current
// --user-data-dir and (when set) the launch profile directory, mirroring
// jumplist.cc's file-static AppendCommonSwitches. `::switches::` is the global
// Chrome switches namespace (distinct from custom_browser::switches).
base::CommandLine MakeBaseCommandLine(
    const base::FilePath& cmd_line_profile_dir) {
  base::CommandLine command_line(base::CommandLine::NO_PROGRAM);
  static constexpr const char* kSwitchNames[] = {::switches::kUserDataDir};
  command_line.CopySwitchesFrom(*base::CommandLine::ForCurrentProcess(),
                                kSwitchNames);
  if (!cmd_line_profile_dir.empty()) {
    command_line.AppendSwitchPath(::switches::kProfileDirectory,
                                  cmd_line_profile_dir);
  }
  return command_line;
}

}  // namespace

std::vector<NexusJumpListTaskSpec> BuildNexusJumpListTaskSpecs(
    policy::IncognitoModeAvailability incognito_availability,
    const base::FilePath& cmd_line_profile_dir) {
  std::vector<NexusJumpListTaskSpec> specs;

  // "New browser window" — --browser-mode launches a normal (non-kNexus)
  // window. Suppressed under forced-incognito, mirroring upstream suppressing
  // its non-incognito "New Window" task.
  if (incognito_availability != policy::IncognitoModeAvailability::kForced) {
    base::CommandLine browser_cl = MakeBaseCommandLine(cmd_line_profile_dir);
    browser_cl.AppendSwitch(switches::kBrowserMode);
    specs.push_back(
        {NexusJumpListTaskSpec::Kind::kBrowserWindow, std::move(browser_cl)});
  }

  // "New Incognito Window" — unchanged from upstream.
  if (incognito_availability != policy::IncognitoModeAvailability::kDisabled) {
    base::CommandLine incognito_cl = MakeBaseCommandLine(cmd_line_profile_dir);
    incognito_cl.AppendSwitch(::switches::kIncognito);
    specs.push_back({NexusJumpListTaskSpec::Kind::kIncognitoWindow,
                     std::move(incognito_cl)});
  }

  return specs;
}

}  // namespace custom_browser
