// Copyright 2026 VellocLabs
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/utility/nexus/shell/install_hints.h"

#include <string>
#include <string_view>

namespace nexus {

namespace {

// One install recipe for one (tool, platform). Each entry is rendered as
// a header line followed by indented shell commands so the model can
// copy/paste them into the Bash tool. Keep entries short — the model only
// needs enough to act, not full prose.
const char* HintForRipgrep(std::string_view platform) {
  if (platform == "windows") {
    return "ripgrep (rg) is not installed. Install one of:\n"
           "  winget install --id BurntSushi.ripgrep.MSVC\n"
           "  choco install ripgrep -y\n"
           "  scoop install ripgrep";
  }
  if (platform == "macos") {
    return "ripgrep (rg) is not installed. Install one of:\n"
           "  brew install ripgrep\n"
           "  port install ripgrep";
  }
  if (platform == "linux") {
    return "ripgrep (rg) is not installed. Install one of:\n"
           "  apt-get install -y ripgrep              # Debian / Ubuntu\n"
           "  dnf install -y ripgrep                  # Fedora / RHEL\n"
           "  pacman -S --noconfirm ripgrep           # Arch\n"
           "  zypper install -y ripgrep               # openSUSE";
  }
  return "";
}

const char* HintForGit(std::string_view platform) {
  if (platform == "windows") {
    return "git is not installed. Install one of:\n"
           "  winget install --id Git.Git\n"
           "  choco install git -y";
  }
  if (platform == "macos") {
    return "git is not installed. Install one of:\n"
           "  xcode-select --install\n"
           "  brew install git";
  }
  if (platform == "linux") {
    return "git is not installed. Install one of:\n"
           "  apt-get install -y git\n"
           "  dnf install -y git\n"
           "  pacman -S --noconfirm git";
  }
  return "";
}

const char* HintForPwsh(std::string_view platform) {
  if (platform == "windows") {
    return "PowerShell 7+ (pwsh) is not installed. Install one of:\n"
           "  winget install --id Microsoft.PowerShell\n"
           "  choco install powershell-core -y";
  }
  if (platform == "macos") {
    return "PowerShell 7+ (pwsh) is not installed. Install one of:\n"
           "  brew install --cask powershell";
  }
  if (platform == "linux") {
    return "PowerShell 7+ (pwsh) is not installed. See "
           "https://learn.microsoft.com/powershell/scripting/install/"
           "installing-powershell-on-linux for distribution-specific steps.";
  }
  return "";
}

const char* HintForBash(std::string_view platform) {
  if (platform == "windows") {
    return "bash is not installed. Install one of:\n"
           "  winget install --id Git.Git              # ships Git Bash\n"
           "  wsl --install                             # Windows Subsystem "
           "for Linux";
  }
  if (platform == "macos") {
    return "bash is not installed. Install via:\n"
           "  brew install bash";
  }
  if (platform == "linux") {
    return "bash should already be present on Linux. If it is missing, "
           "use the system package manager (apt, dnf, pacman, zypper) to "
           "install the `bash` package.";
  }
  return "";
}

}  // namespace

std::string InstallHintFor(std::string_view tool_id,
                           std::string_view platform) {
  if (tool_id == "rg") {
    return HintForRipgrep(platform);
  }
  if (tool_id == "git") {
    return HintForGit(platform);
  }
  if (tool_id == "pwsh" || tool_id == "powershell") {
    return HintForPwsh(platform);
  }
  if (tool_id == "bash" || tool_id == "sh" || tool_id == "zsh") {
    return HintForBash(platform);
  }
  // No hint recorded — caller will fall back to a generic message.
  return std::string();
}

}  // namespace nexus
