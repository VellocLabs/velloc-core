#include "custom_browser/browser/nexus/api/shell/host_shell.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/strings/string_split.h"
#include "build/build_config.h"
#include "custom_browser/common/nexus_runtime_paths.h"

namespace nexus {

namespace {

#if BUILDFLAG(IS_WIN)
constexpr char kPathSeparator = ';';
const char* const kExecutableExtensions[] = {".exe", ".com", ".cmd",
                                             ".bat", ".ps1", ""};
#else
constexpr char kPathSeparator = ':';
const char* const kExecutableExtensions[] = {""};
#endif

#if BUILDFLAG(IS_WIN)
std::optional<base::FilePath> EnvPath(const char* name) {
  std::unique_ptr<base::Environment> env = base::Environment::Create();
  std::optional<std::string> value = env->GetVar(std::string(name));
  if (!value.has_value() || value->empty()) {
    return std::nullopt;
  }
  return base::FilePath::FromUTF8Unsafe(*value);
}

void AppendGitBashCandidatesFromRoot(const base::FilePath& root,
                                     std::vector<base::FilePath>* out) {
  if (root.empty()) return;
  out->push_back(root.AppendASCII("bin").AppendASCII("bash.exe"));
  out->push_back(root.AppendASCII("usr").AppendASCII("bin").AppendASCII("bash.exe"));
}

bool IsGitBashInstalledForWindows() {
  std::vector<base::FilePath> candidates;
  for (const char* env_name : {"NEXUS_BASH_PATH", "VELLOC_BASH_PATH", "SHELL"}) {
    if (std::optional<base::FilePath> p = EnvPath(env_name); p.has_value()) {
      candidates.push_back(*p);
    }
  }
  if (std::optional<base::FilePath> program_files = EnvPath("ProgramFiles")) {
    AppendGitBashCandidatesFromRoot(program_files->AppendASCII("Git"), &candidates);
  }
  if (std::optional<base::FilePath> program_files_x86 = EnvPath("ProgramFiles(x86)")) {
    AppendGitBashCandidatesFromRoot(program_files_x86->AppendASCII("Git"), &candidates);
  }
  if (std::optional<base::FilePath> local_app_data = EnvPath("LOCALAPPDATA")) {
    AppendGitBashCandidatesFromRoot(
        local_app_data->AppendASCII("Programs").AppendASCII("Git"), &candidates);
  }
  AppendGitBashCandidatesFromRoot(
      base::FilePath(FILE_PATH_LITERAL("C:\\Program Files\\Git")), &candidates);
  AppendGitBashCandidatesFromRoot(
      base::FilePath(FILE_PATH_LITERAL("C:\\Program Files (x86)\\Git")),
      &candidates);

  for (const base::FilePath& candidate : candidates) {
    if (base::PathExists(candidate) && !base::DirectoryExists(candidate)) {
      return true;
    }
  }
  return false;
}
#endif

// Synchronous PATH lookup — no spawn, no `--version` probe. MUST run on a
// MayBlock thread (it stats files). Mirrors the PATH-walk in
// ShellResolver::FindOnPath (utility process) but browser-side and probe-
// free, just to answer "is this shell installed?".
bool IsExecutableOnPath(const std::string& tool_id,
                        bool (*rejects)(const base::FilePath&) = nullptr) {
  std::unique_ptr<base::Environment> env = base::Environment::Create();
  std::optional<std::string> path_value = env->GetVar("PATH");
  if (!path_value.has_value() || path_value->empty()) {
    return false;
  }
  for (std::string_view part : base::SplitStringPiece(
           *path_value, std::string(1, kPathSeparator), base::TRIM_WHITESPACE,
           base::SPLIT_WANT_NONEMPTY)) {
    base::FilePath dir = base::FilePath::FromUTF8Unsafe(part);
    for (const char* ext : kExecutableExtensions) {
      base::FilePath candidate = dir.AppendASCII(tool_id + std::string(ext));
      if (base::PathExists(candidate) && !base::DirectoryExists(candidate) &&
          !(rejects && rejects(candidate))) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace

HostShellAvailability ResolveHostShellAvailability() {
  HostShellAvailability shells;
  // The WSL launcher is not a bash (nexus_runtime_paths.h).
  shells.bash =
      IsExecutableOnPath("bash", &custom_browser::IsWslBashLauncher);
#if BUILDFLAG(IS_WIN)
  if (!shells.bash) {
    shells.bash = IsGitBashInstalledForWindows();
  }
#endif
  shells.pwsh_core = IsExecutableOnPath("pwsh");
  shells.windows_powershell = IsExecutableOnPath("powershell");
  return shells;
}

std::string ChooseReportedShellName(const HostShellAvailability& shells,
                                    std::optional<std::string> shell_env,
                                    std::optional<std::string> comspec_env) {
#if BUILDFLAG(IS_WIN)
  if (shells.bash) {
    return "bash";
  }
  if (shells.pwsh_core) {
    return "pwsh";
  }
  if (shells.windows_powershell) {
    return "powershell";
  }
  // No tool shell at all: name what the host has, $SHELL first.
  std::string named = ExtractShellBasename(std::move(shell_env));
  return named.empty() ? ExtractShellBasename(std::move(comspec_env)) : named;
#else
  return ExtractShellBasename(std::move(shell_env));
#endif
}

std::string ChoosePowerShellName(const HostShellAvailability& shells) {
#if BUILDFLAG(IS_WIN)
  if (shells.pwsh_core) {
    return "pwsh";
  }
  if (shells.windows_powershell) {
    return "powershell";
  }
#endif
  return std::string();
}

std::string ResolveReportedShellName() {
  std::unique_ptr<base::Environment> env = base::Environment::Create();
  return ChooseReportedShellName(ResolveHostShellAvailability(),
                                 env->GetVar("SHELL"), env->GetVar("COMSPEC"));
}

std::string ExtractShellBasename(std::optional<std::string> raw) {
  if (!raw.has_value() || raw->empty()) {
    return std::string();
  }
  base::FilePath shell_path = base::FilePath::FromUTF8Unsafe(*raw);
  std::string basename = shell_path.BaseName().AsUTF8Unsafe();
#if BUILDFLAG(IS_WIN)
  // Strip a trailing .exe on Windows so the model sees "bash" not
  // "bash.exe" — the plain shell name the prompt's Shell line reports.
  constexpr char kExeSuffix[] = ".exe";
  constexpr size_t kExeSuffixLen = sizeof(kExeSuffix) - 1;
  if (basename.size() > kExeSuffixLen) {
    const size_t suffix_pos = basename.size() - kExeSuffixLen;
    if (basename.compare(suffix_pos, std::string::npos, kExeSuffix) == 0) {
      basename.resize(suffix_pos);
    }
  }
#endif
  return basename;
}

}  // namespace nexus
