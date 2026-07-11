#include "custom_browser/win/nexus_jumplist_task_specs.h"

#include "base/files/file_path.h"
#include "chrome/common/chrome_switches.h"
#include "components/policy/core/common/policy_pref_names.h"
#include "custom_browser/common/custom_browser_switches.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace custom_browser {
namespace {

using Kind = NexusJumpListTaskSpec::Kind;

// `switches::kBrowserMode` resolves to custom_browser::switches::kBrowserMode
// here; `::switches::` is the global Chrome switches namespace.

TEST(NexusJumpListTaskSpecsTest, EnabledYieldsBrowserAndIncognito) {
  std::vector<NexusJumpListTaskSpec> specs = BuildNexusJumpListTaskSpecs(
      policy::IncognitoModeAvailability::kEnabled, base::FilePath());
  ASSERT_EQ(specs.size(), 2u);

  // Browser window: --browser-mode (→ normal window).
  EXPECT_EQ(specs[0].kind, Kind::kBrowserWindow);
  EXPECT_TRUE(specs[0].command_line.HasSwitch(switches::kBrowserMode));
  EXPECT_FALSE(specs[0].command_line.HasSwitch(::switches::kIncognito));

  // Incognito window: --incognito.
  EXPECT_EQ(specs[1].kind, Kind::kIncognitoWindow);
  EXPECT_TRUE(specs[1].command_line.HasSwitch(::switches::kIncognito));
  EXPECT_FALSE(specs[1].command_line.HasSwitch(switches::kBrowserMode));
}

TEST(NexusJumpListTaskSpecsTest, DisabledDropsIncognito) {
  std::vector<NexusJumpListTaskSpec> specs = BuildNexusJumpListTaskSpecs(
      policy::IncognitoModeAvailability::kDisabled, base::FilePath());
  ASSERT_EQ(specs.size(), 1u);
  EXPECT_EQ(specs[0].kind, Kind::kBrowserWindow);
  EXPECT_TRUE(specs[0].command_line.HasSwitch(switches::kBrowserMode));
}

TEST(NexusJumpListTaskSpecsTest, ForcedKeepsOnlyIncognito) {
  std::vector<NexusJumpListTaskSpec> specs = BuildNexusJumpListTaskSpecs(
      policy::IncognitoModeAvailability::kForced, base::FilePath());
  ASSERT_EQ(specs.size(), 1u);
  EXPECT_EQ(specs[0].kind, Kind::kIncognitoWindow);
  EXPECT_TRUE(specs[0].command_line.HasSwitch(::switches::kIncognito));
}

TEST(NexusJumpListTaskSpecsTest, PropagatesProfileDirectory) {
  const base::FilePath profile_dir(FILE_PATH_LITERAL("Profile 2"));
  std::vector<NexusJumpListTaskSpec> specs = BuildNexusJumpListTaskSpecs(
      policy::IncognitoModeAvailability::kEnabled, profile_dir);
  ASSERT_FALSE(specs.empty());
  for (const NexusJumpListTaskSpec& spec : specs) {
    EXPECT_EQ(
        spec.command_line.GetSwitchValuePath(::switches::kProfileDirectory),
        profile_dir);
  }
}

TEST(NexusJumpListTaskSpecsTest, OmitsProfileDirectoryWhenEmpty) {
  std::vector<NexusJumpListTaskSpec> specs = BuildNexusJumpListTaskSpecs(
      policy::IncognitoModeAvailability::kEnabled, base::FilePath());
  ASSERT_FALSE(specs.empty());
  for (const NexusJumpListTaskSpec& spec : specs) {
    EXPECT_FALSE(spec.command_line.HasSwitch(::switches::kProfileDirectory));
  }
}

}  // namespace
}  // namespace custom_browser
