// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/utility/nexus/shell/safe_command_policy.h"

#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/scoped_temp_dir.h"
#include "base/strings/utf_string_conversions.h"
#include "build/build_config.h"
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

TEST(SafeCommandPolicyTest, ClampTimeoutMs) {
  EXPECT_EQ(SafeCommandPolicy::ClampTimeoutMs(0),
            SafeCommandLimits::kDefaultTimeoutMs);
  EXPECT_EQ(SafeCommandPolicy::ClampTimeoutMs(1000), 1000u);
  EXPECT_EQ(SafeCommandPolicy::ClampTimeoutMs(60u * 60u * 1000u),
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
