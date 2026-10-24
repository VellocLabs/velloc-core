// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/utility/nexus/shell/shell_resolver.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/scoped_environment_variable_override.h"
#include "base/base_paths.h"
#include "base/test/scoped_path_override.h"
#include "base/test/task_environment.h"
#include "build/build_config.h"
#include "custom_browser/common/nexus_runtime_paths.h"
#include "testing/gtest/include/gtest/gtest.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>

#include "base/strings/utf_string_conversions.h"
#include "base/test/test_reg_util_win.h"
#include "base/win/registry.h"
#endif

namespace nexus {
namespace {

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

  base::FilePath CreateManagedBinary(const std::string& id) {
    base::FilePath exe = custom_browser::GetManagedBinaryPath(id);
    CHECK(!exe.empty());
    CHECK(base::CreateDirectory(exe.DirName()));
    CHECK(base::WriteFile(exe, "binary"));
    return exe;
  }

  const base::FilePath& path() const { return home_.GetPath(); }

 private:
  base::ScopedTempDir home_;
  std::unique_ptr<base::ScopedEnvironmentVariableOverride> home_env_;
  std::unique_ptr<base::ScopedEnvironmentVariableOverride> userprofile_env_;
};

// Stands in for the machine and user registry `Path` values for the scope.
class ScopedRegistryPaths {
 public:
  explicit ScopedRegistryPaths(std::vector<std::string> paths) {
    ShellResolver::SetRegistryPathsForTesting(std::move(paths));
  }
  ~ScopedRegistryPaths() { ShellResolver::SetRegistryPathsForTesting(std::nullopt); }
};

base::FilePath WriteFakeTool(const base::FilePath& dir, const std::string& id) {
#if BUILDFLAG(IS_WIN)
  const base::FilePath exe = dir.AppendASCII(id + ".exe");
#else
  const base::FilePath exe = dir.AppendASCII(id);
#endif
  CHECK(base::CreateDirectory(dir));
  CHECK(base::WriteFile(exe, "binary"));
  return exe;
}

ResolvedTool MakeTool(const std::string& id, bool available) {
  ResolvedTool tool;
  tool.id = id;
  tool.available = available;
  if (available) {
    tool.path = base::FilePath(FILE_PATH_LITERAL("/opt/")).AppendASCII(id);
  }
  return tool;
}

TEST(ShellResolverTest, FindOnPathResolvesManagedBinary) {
  // A unique id that won't be on the host PATH, so a hit can only come
  // from the ~/.velloc/bin probe.
  ScopedManagedHome home;
  const std::string id = "nexus-managed-test-bin";
  const base::FilePath exe = home.CreateManagedBinary(id);
  const std::optional<base::FilePath> found = ShellResolver::FindOnPath(id);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(*found, exe);
}

TEST(ShellResolverTest, FindOnPathPrefersBundledRgNextToExe) {
  // The installer ships rg.exe in the version folder, next to chrome.dll; that copy wins over a
  // managed download and over any rg on the host PATH.
  ScopedManagedHome home;
  home.CreateManagedBinary("rg");
  base::ScopedTempDir exe_dir;
  ASSERT_TRUE(exe_dir.CreateUniqueTempDir());
  base::ScopedPathOverride exe_override(base::DIR_MODULE, exe_dir.GetPath(),
                                        /*is_absolute=*/true,
                                        /*create=*/false);
  const base::FilePath bundled = custom_browser::GetBundledBinaryPath("rg");
  ASSERT_EQ(bundled.DirName(), exe_dir.GetPath());
  ASSERT_TRUE(base::WriteFile(bundled, "binary"));
  const std::optional<base::FilePath> found = ShellResolver::FindOnPath("rg");
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(*found, bundled);
}

TEST(ShellResolverTest, GetBundledBinaryPathOnlyForBundledIds) {
  EXPECT_FALSE(custom_browser::GetBundledBinaryPath("rg").empty());
  EXPECT_TRUE(custom_browser::GetBundledBinaryPath("node").empty());
  EXPECT_TRUE(custom_browser::GetBundledBinaryPath("chrome").empty());
}

TEST(ShellResolverTest, FindOnPathReturnsNulloptForAbsentManagedBinary) {
  ScopedManagedHome home;
  EXPECT_FALSE(
      ShellResolver::FindOnPath("nexus-managed-absent-bin").has_value());
}

#if BUILDFLAG(IS_WIN)
TEST(ShellResolverTest, MergePathListsKeepsFirstSeenOrderAndDropsDuplicates) {
  EXPECT_EQ(ShellResolver::MergePathLists(
                {"C:\\a;C:\\b;;", "c:\\B\\;C:\\m; C:\\A", "C:\\u;C:\\m"}),
            "C:\\a;C:\\b;C:\\m;C:\\u");
}

TEST(ShellResolverTest, EffectivePathPutsProcessPathBeforeMachineThenUser) {
  base::ScopedEnvironmentVariableOverride path_env("PATH", "C:\\p1;C:\\shared");
  ScopedRegistryPaths registry({"C:\\machine;c:\\SHARED", "C:\\user;C:\\p1\\"});
  EXPECT_EQ(ShellResolver::EffectivePath(),
            "C:\\p1;C:\\shared;C:\\machine;C:\\user");
}

// Reads the real keys (redirected to a scratch hive): a long machine REG_SZ
// and an expandable user value.
TEST(ShellResolverTest, EffectivePathReadsAndExpandsTheRegistryValues) {
  registry_util::RegistryOverrideManager registry_override;
  ASSERT_NO_FATAL_FAILURE(
      registry_override.OverrideRegistry(HKEY_LOCAL_MACHINE));
  ASSERT_NO_FATAL_FAILURE(
      registry_override.OverrideRegistry(HKEY_CURRENT_USER));
  const std::wstring machine_dir = L"C:\\" + std::wstring(1500, L'm');
  ASSERT_EQ(base::win::RegKey(HKEY_LOCAL_MACHINE,
                              L"SYSTEM\\CurrentControlSet\\Control\\Session "
                              L"Manager\\Environment",
                              KEY_SET_VALUE)
                .WriteValue(L"Path", machine_dir.c_str()),
            ERROR_SUCCESS);
  const std::wstring user_value = L"%NEXUS_REGISTRY_TEST_ROOT%\\bin";
  ASSERT_EQ(base::win::RegKey(HKEY_CURRENT_USER, L"Environment", KEY_SET_VALUE)
                .WriteValue(L"Path", user_value.c_str(),
                            static_cast<DWORD>((user_value.size() + 1) *
                                               sizeof(wchar_t)),
                            REG_EXPAND_SZ),
            ERROR_SUCCESS);
  base::ScopedEnvironmentVariableOverride root_env("NEXUS_REGISTRY_TEST_ROOT",
                                                   "C:\\uvroot");
  base::ScopedEnvironmentVariableOverride path_env("PATH", "C:\\process");

  EXPECT_EQ(ShellResolver::EffectivePath(),
            "C:\\process;" + base::WideToUTF8(machine_dir) + ";C:\\uvroot\\bin");
}

TEST(ShellResolverTest, EffectivePathSkipsTheRegistryUnderTheE2eSeam) {
  registry_util::RegistryOverrideManager registry_override;
  ASSERT_NO_FATAL_FAILURE(
      registry_override.OverrideRegistry(HKEY_CURRENT_USER));
  ASSERT_EQ(base::win::RegKey(HKEY_CURRENT_USER, L"Environment", KEY_SET_VALUE)
                .WriteValue(L"Path", L"C:\\registry-only"),
            ERROR_SUCCESS);
  base::ScopedEnvironmentVariableOverride path_env("PATH", "C:\\process");
  base::ScopedEnvironmentVariableOverride seam("NEXUS_E2E_NO_REGISTRY_PATH",
                                               "1");

  EXPECT_EQ(ShellResolver::EffectivePath(), "C:\\process");
}

TEST(ShellResolverTest, FindOnPathFindsToolOnlyOnTheRegistryPath) {
  // An installer that ran after launch updated only the registry Path.
  base::ScopedTempDir tmp;
  ASSERT_TRUE(tmp.CreateUniqueTempDir());
  const base::FilePath dir = tmp.GetPath().AppendASCII("installed");
  const base::FilePath exe = WriteFakeTool(dir, "nexus-registry-path-bin");
  base::ScopedEnvironmentVariableOverride path_env("PATH", "");
  ScopedRegistryPaths registry({"", dir.AsUTF8Unsafe()});

  const std::optional<base::FilePath> found =
      ShellResolver::FindOnPath("nexus-registry-path-bin");
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(*found, exe);
}
#else
TEST(ShellResolverTest, MergePathListsKeepsFirstSeenOrderAndDropsDuplicates) {
  EXPECT_EQ(ShellResolver::MergePathLists({"/a:/b::", "/b:/m", "/u:/m:/a"}),
            "/a:/b:/m:/u");
}

TEST(ShellResolverTest, EffectivePathIsTheProcessPath) {
  base::ScopedEnvironmentVariableOverride path_env("PATH", "/p1:/p2:/p1");
  EXPECT_EQ(ShellResolver::EffectivePath(), "/p1:/p2:/p1");
}
#endif

TEST(ShellResolverTest, FindOnPathFindsUvInItsInstallDirectories) {
  ScopedManagedHome home;
  base::ScopedTempDir local_app_data;
  ASSERT_TRUE(local_app_data.CreateUniqueTempDir());
  base::ScopedEnvironmentVariableOverride local_app_data_env(
      "LOCALAPPDATA", local_app_data.GetPath().AsUTF8Unsafe());
  base::ScopedEnvironmentVariableOverride path_env("PATH", "");
  ScopedRegistryPaths registry({});

  EXPECT_FALSE(ShellResolver::FindOnPath("uv").has_value());
  EXPECT_FALSE(ShellResolver::FindOnPath("uvx").has_value());

  const base::FilePath uvx = WriteFakeTool(
      home.path().AppendASCII(".local").AppendASCII("bin"), "uvx");
  const base::FilePath uv = WriteFakeTool(
      home.path().AppendASCII(".cargo").AppendASCII("bin"), "uv");
  EXPECT_EQ(ShellResolver::FindOnPath("uvx"), uvx);
  EXPECT_EQ(ShellResolver::FindOnPath("uv"), uv);
  // The uv dirs are not a general fallback.
  WriteFakeTool(home.path().AppendASCII(".local").AppendASCII("bin"),
                "nexus-not-uv-bin");
  EXPECT_FALSE(ShellResolver::FindOnPath("nexus-not-uv-bin").has_value());
}

TEST(ShellResolverTest, FindOnPathFindsCliAgentsInTheirInstallDirectories) {
  ScopedManagedHome home;
  base::ScopedTempDir app_data;
  ASSERT_TRUE(app_data.CreateUniqueTempDir());
  base::ScopedEnvironmentVariableOverride app_data_env(
      "APPDATA", app_data.GetPath().AsUTF8Unsafe());
  base::ScopedEnvironmentVariableOverride path_env("PATH", "");
  ScopedRegistryPaths registry({});

  EXPECT_FALSE(ShellResolver::FindOnPath("claude").has_value());
  EXPECT_FALSE(ShellResolver::FindOnPath("codex").has_value());

  const base::FilePath claude = WriteFakeTool(
      home.path().AppendASCII(".local").AppendASCII("bin"), "claude");
  EXPECT_EQ(ShellResolver::FindOnPath("claude"), claude);
#if BUILDFLAG(IS_WIN)
  const base::FilePath codex =
      WriteFakeTool(app_data.GetPath().AppendASCII("npm"), "codex");
  EXPECT_EQ(ShellResolver::FindOnPath("codex"), codex);
#endif
  // The CLI dirs are not a general fallback.
  WriteFakeTool(home.path().AppendASCII(".local").AppendASCII("bin"),
                "nexus-not-a-cli-bin");
  EXPECT_FALSE(ShellResolver::FindOnPath("nexus-not-a-cli-bin").has_value());
}

#if BUILDFLAG(IS_WIN)
TEST(ShellResolverTest, ResolveToolsFindsCliAgentsWithoutLaunchingThem) {
  ScopedManagedHome home;
  base::ScopedEnvironmentVariableOverride path_env("PATH", "");
  ScopedRegistryPaths registry({});
  const base::FilePath bin = home.path().AppendASCII(".local").AppendASCII("bin");
  const base::FilePath marker = home.path().AppendASCII("launched.txt");
  ASSERT_TRUE(base::CreateDirectory(bin));
  for (const char* id : {"claude", "codex"}) {
    ASSERT_TRUE(base::WriteFile(bin.AppendASCII(std::string(id) + ".cmd"),
                                "@echo 1.0.0\r\n@echo x> \"" +
                                    marker.AsUTF8Unsafe() + "\"\r\n"));
  }

  const std::vector<ResolvedTool> tools =
      ShellResolver::ResolveTools({"claude", "codex"});

  ASSERT_EQ(tools.size(), 2u);
  for (const ResolvedTool& tool : tools) {
    EXPECT_TRUE(tool.available) << tool.id;
    EXPECT_TRUE(tool.version.empty()) << tool.id;
  }
  EXPECT_FALSE(base::PathExists(marker));
}
#endif

#if BUILDFLAG(IS_WIN)
TEST(ShellResolverTest, FindOnPathFindsUvInWinGetLinks) {
  ScopedManagedHome home;
  base::ScopedTempDir local_app_data;
  ASSERT_TRUE(local_app_data.CreateUniqueTempDir());
  base::ScopedEnvironmentVariableOverride local_app_data_env(
      "LOCALAPPDATA", local_app_data.GetPath().AsUTF8Unsafe());
  base::ScopedEnvironmentVariableOverride path_env("PATH", "");
  ScopedRegistryPaths registry({});
  const base::FilePath uv = WriteFakeTool(local_app_data.GetPath()
                                              .AppendASCII("Microsoft")
                                              .AppendASCII("WinGet")
                                              .AppendASCII("Links"),
                                          "uv");
  EXPECT_EQ(ShellResolver::FindOnPath("uv"), uv);
}
#endif

TEST(ShellResolverTest, ApplyResolvedToolsFillsAMissAndKeepsAHit) {
  ResolvedCapabilities caps;
  caps.tools = {MakeTool("git", true), MakeTool("uvx", false)};
  caps.command_allowlist = {"git"};

  ShellResolver::ApplyResolvedTools(
      {MakeTool("git", false), MakeTool("uvx", true)}, &caps);

  ASSERT_EQ(caps.tools.size(), 2u);
  EXPECT_TRUE(caps.tools[0].available) << "a found tool must stay found";
  EXPECT_FALSE(caps.tools[0].path.empty());
  EXPECT_TRUE(caps.tools[1].available);
  EXPECT_EQ(caps.command_allowlist, (std::vector<std::string>{"git", "uvx"}));
}

TEST(ShellResolverTest, ApplyResolvedToolsPicksUpANewlyFoundDefaultShell) {
  ResolvedCapabilities caps;
#if BUILDFLAG(IS_WIN)
  const std::string shell = "cmd";
#else
  const std::string shell = "sh";
#endif
  caps.tools = {MakeTool(shell, false)};
  ShellResolver::ApplyResolvedTools({MakeTool(shell, true)}, &caps);
  ASSERT_TRUE(caps.default_shell.has_value());
  EXPECT_EQ(caps.default_shell->id, shell);
}



#if BUILDFLAG(IS_WIN)
TEST(ShellResolverTest, FindOnPathHonorsExplicitBashPathEnvOverride) {
  base::ScopedTempDir tmp;
  ASSERT_TRUE(tmp.CreateUniqueTempDir());
  base::FilePath bash = tmp.GetPath().AppendASCII("custom-bash.exe");
  ASSERT_TRUE(base::WriteFile(bash, "fake bash"));

  // Only NEXUS_BASH_PATH is under test here; clear the other override vars
  // FindOnPath also consults so the test is hermetic regardless of the
  // shell (e.g. git-bash, which sets SHELL) it happens to be invoked from.
  base::ScopedEnvironmentVariableOverride velloc_bash_path_env(
      "VELLOC_BASH_PATH");
  base::ScopedEnvironmentVariableOverride shell_env("SHELL");
  base::ScopedEnvironmentVariableOverride override_env(
      "NEXUS_BASH_PATH", bash.AsUTF8Unsafe());
  base::ScopedEnvironmentVariableOverride path_env("PATH", "");
  ScopedRegistryPaths registry({});

  std::optional<base::FilePath> found = ShellResolver::FindOnPath("bash");
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(*found, bash);
}
#endif

#if BUILDFLAG(IS_WIN)
TEST(ShellResolverTest, FindOnPathFindsGitBashFromCommonProgramFilesLocation) {
  base::ScopedTempDir tmp;
  ASSERT_TRUE(tmp.CreateUniqueTempDir());
  base::FilePath program_files = tmp.GetPath().AppendASCII("ProgramFiles");
  base::FilePath bash =
      program_files.AppendASCII("Git").AppendASCII("bin").AppendASCII("bash.exe");
  ASSERT_TRUE(base::CreateDirectory(bash.DirName()));
  ASSERT_TRUE(base::WriteFile(bash, "fake bash"));

  // None of the explicit override vars are under test here; clear all four
  // so the ProgramFiles fallback is what actually gets exercised regardless
  // of the shell (e.g. git-bash, which sets SHELL) invoking the test binary.
  base::ScopedEnvironmentVariableOverride nexus_bash_path_env(
      "NEXUS_BASH_PATH");
  base::ScopedEnvironmentVariableOverride velloc_bash_path_env(
      "VELLOC_BASH_PATH");
  base::ScopedEnvironmentVariableOverride shell_env("SHELL");
  base::ScopedEnvironmentVariableOverride program_files_env(
      "ProgramFiles", program_files.AsUTF8Unsafe());
  base::ScopedEnvironmentVariableOverride path_env("PATH", "");
  ScopedRegistryPaths registry({});

  std::optional<base::FilePath> found = ShellResolver::FindOnPath("bash");
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(*found, bash);
}
#endif

#if BUILDFLAG(IS_WIN)
TEST(ShellResolverTest, FindOnPathFindsGitShWhenOnlyGitCmdIsOnPath) {
  // The Git for Windows installer puts only Git\cmd on PATH; sh.exe lives in
  // Git\usr\bin and must still resolve.
  base::ScopedTempDir tmp;
  ASSERT_TRUE(tmp.CreateUniqueTempDir());
  base::FilePath git_root =
      tmp.GetPath().AppendASCII("ProgramFiles").AppendASCII("Git");
  base::FilePath sh =
      git_root.AppendASCII("usr").AppendASCII("bin").AppendASCII("sh.exe");
  ASSERT_TRUE(base::CreateDirectory(sh.DirName()));
  ASSERT_TRUE(base::WriteFile(sh, "fake sh"));
  base::FilePath git_cmd = git_root.AppendASCII("cmd");
  ASSERT_TRUE(base::CreateDirectory(git_cmd));
  ASSERT_TRUE(base::WriteFile(git_cmd.AppendASCII("git.exe"), "fake git"));

  // A bash override must not be returned for `sh`.
  base::FilePath bash = tmp.GetPath().AppendASCII("custom-bash.exe");
  ASSERT_TRUE(base::WriteFile(bash, "fake bash"));
  base::ScopedEnvironmentVariableOverride nexus_bash_path_env(
      "NEXUS_BASH_PATH", bash.AsUTF8Unsafe());
  base::ScopedEnvironmentVariableOverride velloc_bash_path_env(
      "VELLOC_BASH_PATH");
  base::ScopedEnvironmentVariableOverride shell_env("SHELL");
  base::ScopedEnvironmentVariableOverride program_files_env(
      "ProgramFiles", git_root.DirName().AsUTF8Unsafe());
  base::ScopedEnvironmentVariableOverride path_env("PATH",
                                                   git_cmd.AsUTF8Unsafe());
  ScopedRegistryPaths registry({});

  std::optional<base::FilePath> found = ShellResolver::FindOnPath("sh");
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(*found, sh);
}
#endif

TEST(ShellResolverTest, KnownToolIdsIsAlphabetisedAndDeduped) {
  const std::vector<std::string> ids = ShellResolver::KnownToolIds();
  ASSERT_FALSE(ids.empty());
  std::vector<std::string> sorted = ids;
  std::sort(sorted.begin(), sorted.end());
  EXPECT_EQ(sorted, ids) << "KnownToolIds() must be alphabetised";

  std::vector<std::string> unique = ids;
  unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
  EXPECT_EQ(unique.size(), ids.size())
      << "KnownToolIds() must not contain duplicates";
}

TEST(ShellResolverTest, ResolveReportsHostPlatform) {
  base::test::TaskEnvironment task_environment;
  const ResolvedCapabilities caps = ShellResolver::Resolve();

#if BUILDFLAG(IS_WIN)
  EXPECT_EQ(caps.platform, "windows");
#elif BUILDFLAG(IS_MAC)
  EXPECT_EQ(caps.platform, "macos");
#elif BUILDFLAG(IS_LINUX)
  EXPECT_EQ(caps.platform, "linux");
#endif
}

TEST(ShellResolverTest, ResolveReturnsEntryForEveryKnownTool) {
  base::test::TaskEnvironment task_environment;
  const ResolvedCapabilities caps = ShellResolver::Resolve();
  const std::vector<std::string> known = ShellResolver::KnownToolIds();
  ASSERT_EQ(caps.tools.size(), known.size())
      << "Every probed tool must appear in `tools`, available or not";

  for (size_t i = 0; i < known.size(); ++i) {
    EXPECT_EQ(caps.tools[i].id, known[i]);
    if (!caps.tools[i].available) {
      // Per the contract: when not available, path / version are empty.
      EXPECT_TRUE(caps.tools[i].path.empty());
      EXPECT_TRUE(caps.tools[i].version.empty());
    } else {
      EXPECT_FALSE(caps.tools[i].path.empty())
          << "Available tool '" << caps.tools[i].id << "' must have a path";
    }
  }
}

TEST(ShellResolverTest, CommandAllowlistMirrorsAvailableTools) {
  // Slice 2 contract: every tool reported as available appears in the
  // allowlist exactly once, in KnownToolIds() order; tools that are
  // unavailable are *not* in the allowlist.
  base::test::TaskEnvironment task_environment;
  const ResolvedCapabilities caps = ShellResolver::Resolve();

  std::vector<std::string> expected;
  for (const ResolvedTool& tool : caps.tools) {
    if (tool.available) {
      expected.push_back(tool.id);
    }
  }
  EXPECT_EQ(caps.command_allowlist, expected);

  for (const std::string& id : caps.command_allowlist) {
    const auto it = std::ranges::find_if(
        caps.tools, [&](const ResolvedTool& t) { return t.id == id; });
    ASSERT_NE(it, caps.tools.end());
    EXPECT_TRUE(it->available)
        << "Allowlist entry '" << id << "' must be marked available";
  }
}

TEST(ShellResolverTest, DefaultShellIsOneOfTheKnownIds) {
  base::test::TaskEnvironment task_environment;
  const ResolvedCapabilities caps = ShellResolver::Resolve();
  if (!caps.default_shell.has_value()) {
    GTEST_SKIP() << "Host has no usable shell on PATH; nothing to verify";
  }

  const std::vector<std::string> known = ShellResolver::KnownToolIds();
  EXPECT_NE(std::ranges::find(known, caps.default_shell->id), known.end());
  EXPECT_TRUE(caps.default_shell->available);
  EXPECT_FALSE(caps.default_shell->path.empty());
}

}  // namespace
}  // namespace nexus
