// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/installer/uninstall_data.h"

#include <memory>
#include <optional>
#include <string>

#include "base/command_line.h"
#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/process/launch.h"
#include "base/process/process.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace custom_browser::installer {
namespace {

// A real junction, made the way a user (or a tool) makes one. base's
// FilePathReparsePoint test helper keeps a handle open on the link for its
// lifetime, which turns a delete into a pending delete; mklink closes it.
bool MakeJunction(const base::FilePath& link, const base::FilePath& target) {
  base::CommandLine cmd(base::FilePath(L"cmd.exe"));
  cmd.AppendArg("/c");
  cmd.AppendArg("mklink");
  cmd.AppendArg("/J");
  cmd.AppendArgPath(link);
  cmd.AppendArgPath(target);
  base::LaunchOptions options;
  options.start_hidden = true;
  base::Process process = base::LaunchProcess(cmd, options);
  int exit_code = -1;
  return process.IsValid() && process.WaitForExit(&exit_code) &&
         exit_code == 0 && base::IsLink(link);
}

class DeleteNexusRuntimeDataTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(home_.CreateUniqueTempDir());
    ASSERT_TRUE(outside_.CreateUniqueTempDir());
    env_ = base::Environment::Create();
    saved_profile_ = env_->GetVar("USERPROFILE");
    ASSERT_TRUE(env_->SetVar("USERPROFILE", home_.GetPath().AsUTF8Unsafe()));
  }

  void TearDown() override {
    if (saved_profile_.has_value()) {
      env_->SetVar("USERPROFILE", *saved_profile_);
    } else {
      env_->UnSetVar("USERPROFILE");
    }
  }

  base::FilePath RuntimeDir() const {
    return home_.GetPath().AppendASCII(".velloc");
  }

  static void Touch(const base::FilePath& path) {
    ASSERT_TRUE(base::CreateDirectory(path.DirName()));
    ASSERT_TRUE(base::WriteFile(path, "x"));
  }

  base::ScopedTempDir home_;
  base::ScopedTempDir outside_;
  std::unique_ptr<base::Environment> env_;
  std::optional<std::string> saved_profile_;
};

TEST_F(DeleteNexusRuntimeDataTest, DeletesTheRuntimeDirAndOnlyIt) {
  Touch(RuntimeDir().AppendASCII("settings.json"));
  Touch(RuntimeDir().AppendASCII("projects").AppendASCII("p").AppendASCII(
      "s.jsonl"));
  Touch(RuntimeDir().AppendASCII("bin").AppendASCII("rg").AppendASCII(
      "rg.exe"));
  const base::FilePath sibling = home_.GetPath().AppendASCII("keep.txt");
  Touch(sibling);

  EXPECT_TRUE(DeleteNexusRuntimeData());

  EXPECT_FALSE(base::PathExists(RuntimeDir()));
  EXPECT_TRUE(base::PathExists(sibling));
}

TEST_F(DeleteNexusRuntimeDataTest, NothingToDeleteIsSuccess) {
  EXPECT_TRUE(DeleteNexusRuntimeData());
  EXPECT_TRUE(base::PathExists(home_.GetPath()));
}

TEST_F(DeleteNexusRuntimeDataTest, DoesNotFollowAJunctionInside) {
  const base::FilePath project_file =
      outside_.GetPath().AppendASCII("project.txt");
  Touch(project_file);
  Touch(RuntimeDir().AppendASCII("settings.json"));
  ASSERT_TRUE(MakeJunction(RuntimeDir().AppendASCII("linked-project"),
                           outside_.GetPath()));

  EXPECT_TRUE(DeleteNexusRuntimeData());

  EXPECT_FALSE(base::PathExists(RuntimeDir()));
  EXPECT_TRUE(base::PathExists(project_file));
}

TEST_F(DeleteNexusRuntimeDataTest, RemovesOnlyTheLinkWhenTheDirIsAJunction) {
  const base::FilePath relocated_file =
      outside_.GetPath().AppendASCII("settings.json");
  Touch(relocated_file);
  ASSERT_TRUE(MakeJunction(RuntimeDir(), outside_.GetPath()));

  EXPECT_TRUE(DeleteNexusRuntimeData());

  EXPECT_FALSE(base::PathExists(RuntimeDir()));
  EXPECT_TRUE(base::PathExists(relocated_file));
}

}  // namespace
}  // namespace custom_browser::installer
