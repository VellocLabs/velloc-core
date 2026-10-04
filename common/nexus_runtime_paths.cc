// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/common/nexus_runtime_paths.h"

#include <algorithm>
#include <iterator>
#include <optional>
#include <string>

#include "base/base_paths.h"
#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/path_service.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "build/build_config.h"
#include "custom_browser/common/branding_constants.h"

namespace custom_browser {

namespace {

// Binaries the Windows installer drops into the version folder, next to
// chrome.dll (custom_browser/installer/custom_browser_installer_wrapper.nsi).
constexpr std::string_view kBundledBinaryIds[] = {"rg"};

// "Velloc" -> ".velloc". Lowercased dotfile, matching the guest's
// `RUNTIME_DIR_NAME` (branding/runtimeDir.ts). Uses the stable path
// component (not the display name) so rebranding the UI never moves
// the on-disk directory.
std::string RuntimeDirName() {
  return base::StrCat(
      {".", base::ToLowerASCII(std::string(kCustomBrowserBrandPathComponent))});
}

}  // namespace

base::FilePath GetNexusUserHomeDir() {
  // Env-first so test isolation (which overrides HOME/USERPROFILE) and the
  // real user home agree with NexusFsHost::UserHome. base::DIR_HOME on
  // Windows consults SHGetKnownFolderPath, which ignores the env override,
  // so we must check the env vars before falling back to PathService.
  std::unique_ptr<base::Environment> env = base::Environment::Create();
#if BUILDFLAG(IS_WIN)
  std::optional<std::string> profile = env->GetVar("USERPROFILE");
  if (profile.has_value() && !profile->empty()) {
    return base::FilePath::FromUTF8Unsafe(*profile);
  }
#endif
  std::optional<std::string> home_env = env->GetVar("HOME");
  if (home_env.has_value() && !home_env->empty()) {
    return base::FilePath::FromUTF8Unsafe(*home_env);
  }

  base::FilePath home;
  if (base::PathService::Get(base::DIR_HOME, &home)) {
    return home;
  }
  return base::FilePath();
}

base::FilePath GetNexusRuntimeDir() {
  base::FilePath home = GetNexusUserHomeDir();
  if (home.empty()) {
    return base::FilePath();
  }
  return home.AppendASCII(RuntimeDirName());
}

base::FilePath GetNexusManagedBinDir() {
  base::FilePath runtime = GetNexusRuntimeDir();
  if (runtime.empty()) {
    return base::FilePath();
  }
  return runtime.AppendASCII("bin");
}

base::FilePath GetManagedBinaryPath(std::string_view id) {
  base::FilePath bin_dir = GetNexusManagedBinDir();
  if (bin_dir.empty() || id.empty()) {
    return base::FilePath();
  }
  std::string exe(id);
#if BUILDFLAG(IS_WIN)
  exe += ".exe";
#endif
  return bin_dir.AppendASCII(std::string(id)).AppendASCII(exe);
}

bool IsUnderManagedBinDir(const base::FilePath& path) {
  base::FilePath bin_dir = GetNexusManagedBinDir();
  if (bin_dir.empty() || path.empty()) {
    return false;
  }
  // Compare on the absolute, `..`-free forms so a path like
  // `~/.velloc/bin/../../evil` can't masquerade as managed. We don't
  // touch the filesystem here (no realpath) — the policy caller pairs
  // this with base::PathExists.
  base::FilePath managed = bin_dir.StripTrailingSeparators();
  base::FilePath candidate = path.StripTrailingSeparators();
  if (candidate.ReferencesParent() || managed.ReferencesParent()) {
    return false;
  }
  return managed == candidate || managed.IsParent(candidate);
}

base::FilePath GetBundledBinaryPath(std::string_view id) {
  if (std::ranges::find(kBundledBinaryIds, id) == std::end(kBundledBinaryIds)) {
    return base::FilePath();
  }
  // DIR_MODULE is chrome.dll's directory — the installed version folder
  // (Application\<version>\), in the browser and utility processes alike.
  base::FilePath module_dir;
  if (!base::PathService::Get(base::DIR_MODULE, &module_dir)) {
    return base::FilePath();
  }
  std::string exe(id);
#if BUILDFLAG(IS_WIN)
  exe += ".exe";
#endif
  return module_dir.AppendASCII(exe);
}

bool IsBundledBinaryPath(const base::FilePath& path) {
  if (path.empty() || path.ReferencesParent()) {
    return false;
  }
  for (std::string_view id : kBundledBinaryIds) {
    base::FilePath bundled = GetBundledBinaryPath(id);
    if (!bundled.empty() && bundled == path.StripTrailingSeparators()) {
      return true;
    }
  }
  return false;
}

}  // namespace custom_browser
