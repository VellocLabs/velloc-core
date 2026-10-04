// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/utility/nexus/shell/safe_command_policy.h"

#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/scoped_environment_variable_override.h"
#include "base/base_paths.h"
#include "base/test/scoped_path_override.h"
#include "base/strings/utf_string_conversions.h"
#include "build/build_config.h"
#include "custom_browser/common/nexus_runtime_paths.h"
#include "custom_browser/utility/nexus/shell/shell_resolver.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

ResolvedCapabilities BuildFakeCaps() {
  ResolvedCapabilities caps;
  caps.platform = "test";

  ResolvedTool ok_tool;
  ok_tool.id = "rg";
#if BUILDFLAG(IS_WIN)
  ok_tool.path = base::FilePath(L"C:\\fake\\rg.exe");
#else
  ok_tool.path = base::FilePath("/fake/bin/rg");
#endif
  ok_tool.version = "rg 14.0.0";
  ok_tool.available = true;
  caps.tools.push_back(ok_tool);

  ResolvedTool absent_tool;
  absent_tool.id = "git";
  absent_tool.available = false;
  caps.tools.push_back(absent_tool);

  ResolvedTool not_allowlisted;
  not_allowlisted.id = "bash";
#if BUILDFLAG(IS_WIN)
  not_allowlisted.path = base::FilePath(L"C:\\fake\\bash.exe");
#else
  not_allowlisted.path = base::FilePath("/fake/bin/bash");
#endif
  not_allowlisted.available = true;
  caps.tools.push_back(not_allowlisted);

  // bash is intentionally NOT in the allowlist even though it's available
  // — exercises the "available but policy-blocked" branch.
  caps.command_allowlist = {"rg"};
  caps.default_shell = not_allowlisted;
  return caps;
}

TEST(SafeCommandPolicyTest, ResolveCommandAcceptsAllowlistedToolId) {
  const ResolvedCapabilities caps = BuildFakeCaps();
  PolicyDecision decision = SafeCommandPolicy::ResolveCommand("rg", caps);
  EXPECT_TRUE(decision.ok()) << decision.error_message;
  EXPECT_EQ(decision.resolved_path, caps.tools[0].path);
}

TEST(SafeCommandPolicyTest, ResolveCommandRejectsAvailableButNonAllowlistedId) {
  const ResolvedCapabilities caps = BuildFakeCaps();
  PolicyDecision decision = SafeCommandPolicy::ResolveCommand("bash", caps);
  ASSERT_FALSE(decision.ok());
  EXPECT_EQ(decision.error_code, "COMMAND_NOT_ALLOWED");
}

TEST(SafeCommandPolicyTest, ResolveCommandRejectsUnknownId) {
  const ResolvedCapabilities caps = BuildFakeCaps();
  PolicyDecision decision =
      SafeCommandPolicy::ResolveCommand("malicious-tool", caps);
  ASSERT_FALSE(decision.ok());
  EXPECT_EQ(decision.error_code, "COMMAND_NOT_ALLOWED");
}

TEST(SafeCommandPolicyTest, ResolveCommandReportsBinaryNotFoundForKnownAbsent) {
  // A known tool id that the resolver tracked but couldn't locate on
  // PATH should return BINARY_NOT_FOUND rather than COMMAND_NOT_ALLOWED,
  // and surface a non-empty install hint so the model can act on it via
  // the Bash tool. (Slice 6.)
  ResolvedCapabilities caps = BuildFakeCaps();
  caps.platform = "linux";
  caps.command_allowlist.push_back("git");
  PolicyDecision decision =
      SafeCommandPolicy::ResolveCommand("git", caps);
  ASSERT_FALSE(decision.ok());
  EXPECT_EQ(decision.error_code, "BINARY_NOT_FOUND");
  EXPECT_FALSE(decision.install_hint.empty())
      << "git is a known tool id; install_hint should be populated for it";
}

TEST(SafeCommandPolicyTest, ResolveCommandReportsBinaryNotFoundEvenWithoutAllowlistEntry) {
  // The resolver records every KnownToolIds() entry in `tools[]` even
  // when not on PATH; the `available == false` branch should still be
  // detected as BINARY_NOT_FOUND because the tool id is recognised.
  ResolvedCapabilities caps = BuildFakeCaps();
  caps.platform = "macos";
  PolicyDecision decision = SafeCommandPolicy::ResolveCommand("git", caps);
  ASSERT_FALSE(decision.ok());
  EXPECT_EQ(decision.error_code, "BINARY_NOT_FOUND");
  EXPECT_FALSE(decision.install_hint.empty());
}

TEST(SafeCommandPolicyTest, ResolveCommandBinaryNotFoundWithEmptyHintIsTolerated) {
  // Unknown-but-allowlisted tool id with no install-hint coverage: still
  // BINARY_NOT_FOUND, but `install_hint` is empty and the guest layer
  // falls back to a generic message.
  ResolvedCapabilities caps = BuildFakeCaps();
  caps.platform = "linux";
  caps.command_allowlist.push_back("magic-unknown-tool");
  PolicyDecision decision =
      SafeCommandPolicy::ResolveCommand("magic-unknown-tool", caps);
  ASSERT_FALSE(decision.ok());
  EXPECT_EQ(decision.error_code, "BINARY_NOT_FOUND");
  EXPECT_TRUE(decision.install_hint.empty());
}

TEST(SafeCommandPolicyTest, ResolveCommandAcceptsAbsoluteMatchingPath) {
  const ResolvedCapabilities caps = BuildFakeCaps();
  PolicyDecision decision = SafeCommandPolicy::ResolveCommand(
      caps.tools[0].path.AsUTF8Unsafe(), caps);
  EXPECT_TRUE(decision.ok()) << decision.error_message;
  EXPECT_EQ(decision.resolved_path, caps.tools[0].path);
}

TEST(SafeCommandPolicyTest, ResolveCommandRejectsAbsolutePathNotMatchingAny) {
  const ResolvedCapabilities caps = BuildFakeCaps();
#if BUILDFLAG(IS_WIN)
  PolicyDecision decision =
      SafeCommandPolicy::ResolveCommand("C:\\Windows\\System32\\evil.exe",
                                        caps);
#else
  PolicyDecision decision =
      SafeCommandPolicy::ResolveCommand("/usr/local/bin/evil", caps);
#endif
  ASSERT_FALSE(decision.ok());
  EXPECT_EQ(decision.error_code, "COMMAND_NOT_ALLOWED");
}

// Points the runtime-path helper at a throwaway home so the managed-bin
// dir (~/.velloc/bin) lands inside a ScopedTempDir for the test.
class ScopedManagedHome {
 public:
  ScopedManagedHome() {
    CHECK(home_.CreateUniqueTempDir());
    const std::string path = home_.GetPath().AsUTF8Unsafe();
    home_env_ = std::make_unique<base::ScopedEnvironmentVariableOverride>(
        "HOME", path);
    userprofile_env_ =
        std::make_unique<base::ScopedEnvironmentVariableOverride>("USERPROFILE",
                                                                  path);
  }

  // Creates ~/.velloc/bin/<id>/<id>[.exe] and returns its absolute path.
  base::FilePath CreateManagedBinary(const std::string& id) {
    base::FilePath exe = custom_browser::GetManagedBinaryPath(id);
    CHECK(!exe.empty());
    CHECK(base::CreateDirectory(exe.DirName()));
    CHECK(base::WriteFile(exe, "binary"));
    return exe;
  }

 private:
  base::ScopedTempDir home_;
  std::unique_ptr<base::ScopedEnvironmentVariableOverride> home_env_;
  std::unique_ptr<base::ScopedEnvironmentVariableOverride> userprofile_env_;
};

TEST(SafeCommandPolicyTest, AcceptsExistingAbsolutePathUnderManagedBinDir) {
  // A freshly-downloaded binary under ~/.velloc/bin is execable by its
  // absolute path even though the cached capabilities don't list it —
  // this is the seam that lets the first Grep run rg right after the
  // installer writes it.
  ScopedManagedHome home;
  const base::FilePath rg = home.CreateManagedBinary("rg");
  const ResolvedCapabilities caps = BuildFakeCaps();
  PolicyDecision decision =
      SafeCommandPolicy::ResolveCommand(rg.AsUTF8Unsafe(), caps);
  EXPECT_TRUE(decision.ok()) << decision.error_message;
  EXPECT_EQ(decision.resolved_path, rg);
}

TEST(SafeCommandPolicyTest, RejectsAbsolutePathUnderManagedBinDirThatIsAbsent) {
  // The trust is "under managed-bin AND exists" — a non-existent path in
  // the managed dir must still be rejected (no blind allow of the dir).
  ScopedManagedHome home;
  const base::FilePath rg = custom_browser::GetManagedBinaryPath("rg");
  ASSERT_FALSE(rg.empty());
  const ResolvedCapabilities caps = BuildFakeCaps();
  PolicyDecision decision =
      SafeCommandPolicy::ResolveCommand(rg.AsUTF8Unsafe(), caps);
  ASSERT_FALSE(decision.ok());
  EXPECT_EQ(decision.error_code, "COMMAND_NOT_ALLOWED");
}

TEST(SafeCommandPolicyTest, AcceptsExistingBundledRgNextToExe) {
  // The installer-shipped rg.exe next to chrome.dll is trusted like a
  // managed download; a sibling file that is not a bundled id is not.
  base::ScopedTempDir exe_dir;
  ASSERT_TRUE(exe_dir.CreateUniqueTempDir());
  base::ScopedPathOverride exe_override(base::DIR_MODULE, exe_dir.GetPath(),
                                        /*is_absolute=*/true,
                                        /*create=*/false);
  const ResolvedCapabilities caps = BuildFakeCaps();
  const base::FilePath rg = custom_browser::GetBundledBinaryPath("rg");
  ASSERT_FALSE(rg.empty());

  // Absent -> rejected (no blind allow of the exe dir).
  EXPECT_FALSE(SafeCommandPolicy::ResolveCommand(rg.AsUTF8Unsafe(), caps).ok());

  ASSERT_TRUE(base::WriteFile(rg, "binary"));
  PolicyDecision decision =
      SafeCommandPolicy::ResolveCommand(rg.AsUTF8Unsafe(), caps);
  EXPECT_TRUE(decision.ok()) << decision.error_message;
  EXPECT_EQ(decision.resolved_path, rg);

  const base::FilePath other = exe_dir.GetPath().AppendASCII("evil.exe");
  ASSERT_TRUE(base::WriteFile(other, "binary"));
  PolicyDecision rejected =
      SafeCommandPolicy::ResolveCommand(other.AsUTF8Unsafe(), caps);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error_code, "COMMAND_NOT_ALLOWED");
}

TEST(SafeCommandPolicyTest, ResolveCommandRejectsEmpty) {
  const ResolvedCapabilities caps = BuildFakeCaps();
  PolicyDecision decision = SafeCommandPolicy::ResolveCommand("", caps);
  ASSERT_FALSE(decision.ok());
  EXPECT_EQ(decision.error_code, "COMMAND_NOT_ALLOWED");
}

TEST(SafeCommandPolicyTest, ValidateCwdRejectsRelativePath) {
  std::string reason;
  EXPECT_FALSE(
      SafeCommandPolicy::ValidateCwd(base::FilePath(), &reason));
  EXPECT_FALSE(reason.empty());
#if BUILDFLAG(IS_WIN)
  EXPECT_FALSE(SafeCommandPolicy::ValidateCwd(
      base::FilePath(L"relative\\path"), &reason));
#else
  EXPECT_FALSE(SafeCommandPolicy::ValidateCwd(
      base::FilePath("relative/path"), &reason));
#endif
}

TEST(SafeCommandPolicyTest, ValidateCwdAcceptsRealDirectory) {
  base::ScopedTempDir tmp;
  ASSERT_TRUE(tmp.CreateUniqueTempDir());
  std::string reason;
  EXPECT_TRUE(SafeCommandPolicy::ValidateCwd(tmp.GetPath(), &reason))
      << reason;
}

TEST(SafeCommandPolicyTest, ValidateCwdRejectsNonExistent) {
  base::ScopedTempDir tmp;
  ASSERT_TRUE(tmp.CreateUniqueTempDir());
  std::string reason;
  EXPECT_FALSE(SafeCommandPolicy::ValidateCwd(
      tmp.GetPath().AppendASCII("does-not-exist"), &reason));
  EXPECT_FALSE(reason.empty());
}

TEST(SafeCommandPolicyTest, BuildSafeEnvironmentDropsForbiddenExtras) {
  std::vector<EnvOverride> extras = {
      {"LD_PRELOAD", "/tmp/evil.so"},
      {"DYLD_INSERT_LIBRARIES", "/tmp/evil.dylib"},
      {"NODE_OPTIONS", "--inspect"},
      {"MY_CUSTOM_VAR", "value"},
  };
  base::EnvironmentMap env = SafeCommandPolicy::BuildSafeEnvironment(extras);

#if BUILDFLAG(IS_WIN)
  const auto custom_key = base::UTF8ToWide(std::string("MY_CUSTOM_VAR"));
  const auto forbidden_keys = std::vector<std::wstring>{
      L"LD_PRELOAD", L"DYLD_INSERT_LIBRARIES", L"NODE_OPTIONS"};
#else
  const auto custom_key = std::string("MY_CUSTOM_VAR");
  const auto forbidden_keys = std::vector<std::string>{
      "LD_PRELOAD", "DYLD_INSERT_LIBRARIES", "NODE_OPTIONS"};
#endif

  EXPECT_NE(env.find(custom_key), env.end())
      << "Custom (non-forbidden) extra var must be present";
  for (const auto& key : forbidden_keys) {
    EXPECT_EQ(env.find(key), env.end())
        << "Forbidden var must be dropped from sanitised env";
  }
}

TEST(SafeCommandPolicyTest, InheritedEnvNamesIncludesPathext) {
  // Regression: PATHEXT must be inherited so cmd / PowerShell can resolve a
  // bare executable name (`git` -> `git.exe`) on Windows. Without it EVERY bare
  // PATH command ("git", "node", "npm", "python") fails with "not recognized"
  // even though the binary is on PATH — the real cause of the project-folder
  // branch chip staying empty on a host with git but no git-bash (PowerShell).
  bool found = false;
  for (const std::string& name : SafeCommandPolicy::InheritedEnvNames()) {
    if (name == "PATHEXT") {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found)
      << "PATHEXT must be inherited for Windows bare-name command resolution";
}


#if BUILDFLAG(IS_WIN)
TEST(SafeCommandPolicyTest, AugmentsPathForGitBashResolvedCommand) {
  base::FilePath git_root(FILE_PATH_LITERAL("C:\\Program Files\\Git"));
  base::FilePath bash = git_root.AppendASCII("bin").AppendASCII("bash.exe");
  base::EnvironmentMap env;
  env[base::UTF8ToWide(std::string("PATH"))] = base::UTF8ToWide(std::string("C:\\Windows\\System32"));

  SafeCommandPolicy::AugmentEnvironmentForResolvedCommand("bash", bash, &env);

  const std::string path = base::WideToUTF8(env[base::UTF8ToWide(std::string("PATH"))]);
  EXPECT_NE(path.find("C:\\Program Files\\Git\\usr\\bin"), std::string::npos);
  EXPECT_NE(path.find("C:\\Program Files\\Git\\mingw64\\bin"), std::string::npos);
  EXPECT_NE(path.find("C:\\Program Files\\Git\\bin"), std::string::npos);
  EXPECT_NE(path.find("C:\\Program Files\\Git\\cmd"), std::string::npos);
  EXPECT_NE(path.find("C:\\Windows\\System32"), std::string::npos);
  EXPECT_EQ(base::WideToUTF8(env[base::UTF8ToWide(std::string("SHELL"))]),
            bash.AsUTF8Unsafe());
}
#endif

TEST(SafeCommandPolicyTest, ClampTimeoutMs) {
  EXPECT_EQ(SafeCommandPolicy::ClampTimeoutMs(0),
            SafeCommandLimits::kDefaultTimeoutMs);
  EXPECT_EQ(SafeCommandPolicy::ClampTimeoutMs(1000), 1000u);
  EXPECT_EQ(SafeCommandPolicy::ClampTimeoutMs(60u * 60u * 1000u),
            SafeCommandLimits::kMaxTimeoutMs);
}

TEST(SafeCommandPolicyTest, ClampTimeoutMsNoCapSentinelPassesThrough) {
  // The no-timeout-cap sentinel (an auto-backgrounded command that must run to
  // completion) is passed through UNCLAMPED — NOT lowered to kMaxTimeoutMs — so a
  // long build is never killed at the 10-minute ceiling. A value one below the
  // sentinel is still clamped normally, proving the passthrough is exact.
  EXPECT_EQ(
      SafeCommandPolicy::ClampTimeoutMs(SafeCommandLimits::kNoTimeoutCapSentinelMs),
      SafeCommandLimits::kNoTimeoutCapSentinelMs);
  EXPECT_EQ(SafeCommandPolicy::ClampTimeoutMs(
                SafeCommandLimits::kNoTimeoutCapSentinelMs - 1u),
            SafeCommandLimits::kMaxTimeoutMs);
}

TEST(SafeCommandPolicyTest, ClampMaxOutputBytes) {
  EXPECT_EQ(SafeCommandPolicy::ClampMaxOutputBytes(0),
            SafeCommandLimits::kDefaultMaxOutputBytes);
  EXPECT_EQ(SafeCommandPolicy::ClampMaxOutputBytes(64u * 1024u), 64u * 1024u);
  EXPECT_EQ(SafeCommandPolicy::ClampMaxOutputBytes(1024u * 1024u * 1024u),
            SafeCommandLimits::kMaxMaxOutputBytes);
}

}  // namespace
}  // namespace nexus
