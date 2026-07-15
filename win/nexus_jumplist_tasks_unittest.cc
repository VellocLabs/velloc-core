#include "custom_browser/win/nexus_jumplist_task_specs.h"

#include "base/files/file_path.h"
#include "components/policy/core/common/policy_pref_names.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace custom_browser {
namespace {

// The Nexus product deliberately exposes no taskbar Jump List "Tasks" — neither
// the "New browser window" (--browser-mode) nor the "New Incognito Window"
// escape-hatch. These cases lock that in: the spec producer yields nothing for
// every incognito policy and regardless of the launch profile directory. They
// are the regression guard against those browser tasks reappearing.

TEST(NexusJumpListTaskSpecsTest, NoTasksForEnabledIncognito) {
  EXPECT_TRUE(BuildNexusJumpListTaskSpecs(
                  policy::IncognitoModeAvailability::kEnabled, base::FilePath())
                  .empty());
}

TEST(NexusJumpListTaskSpecsTest, NoTasksForDisabledIncognito) {
  EXPECT_TRUE(BuildNexusJumpListTaskSpecs(
                  policy::IncognitoModeAvailability::kDisabled, base::FilePath())
                  .empty());
}

TEST(NexusJumpListTaskSpecsTest, NoTasksForForcedIncognito) {
  EXPECT_TRUE(BuildNexusJumpListTaskSpecs(
                  policy::IncognitoModeAvailability::kForced, base::FilePath())
                  .empty());
}

TEST(NexusJumpListTaskSpecsTest, NoTasksWithLaunchProfileDirectory) {
  EXPECT_TRUE(BuildNexusJumpListTaskSpecs(
                  policy::IncognitoModeAvailability::kEnabled,
                  base::FilePath(FILE_PATH_LITERAL("Profile 2")))
                  .empty());
}

}  // namespace
}  // namespace custom_browser
