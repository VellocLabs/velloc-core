// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/binaries/nexus_binary_registry.h"

#include <array>
#include <string>
#include <utility>

#include "base/containers/span.h"
#include "build/build_config.h"
#include "custom_browser/browser/nexus/binaries/nexus_binary_config_registry.h"

namespace nexus {

namespace {

// ripgrep 14.1.1 — backs the `Grep` tool (and a bare `rg` Bash call).
// SHA-256 values are of the upstream BurntSushi/ripgrep release assets.
// The member path is the executable inside each archive
// (`ripgrep-<ver>-<target>/rg[.exe]`).
constexpr std::array<BinaryAsset, 6> kRipgrepAssets = {{
    {HostPlatform::kWindows, HostArch::kX64,
     "https://github.com/BurntSushi/ripgrep/releases/download/14.1.1/"
     "ripgrep-14.1.1-x86_64-pc-windows-msvc.zip",
     "d0f534024c42afd6cb4d38907c25cd2b249b79bbe6cc1dbee8e3e37c2b6e25a1",
     BinaryArchiveFormat::kZip,
     "ripgrep-14.1.1-x86_64-pc-windows-msvc/rg.exe"},
    {HostPlatform::kWindows, HostArch::kArm64,
     "https://github.com/BurntSushi/ripgrep/releases/download/14.1.1/"
     "ripgrep-14.1.1-aarch64-pc-windows-msvc.zip",
     "0019dfc4b32d63c1392aa264aed2253c1e0c2fb09216f8e2cc269bbfb8bb49b5",
     BinaryArchiveFormat::kZip,
     "ripgrep-14.1.1-aarch64-pc-windows-msvc/rg.exe"},
    {HostPlatform::kLinux, HostArch::kX64,
     "https://github.com/BurntSushi/ripgrep/releases/download/14.1.1/"
     "ripgrep-14.1.1-x86_64-unknown-linux-musl.tar.gz",
     "4cf9f2741e6c465ffdb7c26f38056a59e2a2544b51f7cc128ef28337eeae4d8e",
     BinaryArchiveFormat::kTarGz,
     "ripgrep-14.1.1-x86_64-unknown-linux-musl/rg"},
    // NOTE: arm64 Linux is glibc-linked — ripgrep 14.1.1 ships no
    // aarch64 musl (static) asset, unlike x64 above. So this needs a
    // glibc host (fine on mainstream distros; a musl-only arm64 host
    // such as Alpine falls back to the BINARY_NOT_FOUND install hint).
    {HostPlatform::kLinux, HostArch::kArm64,
     "https://github.com/BurntSushi/ripgrep/releases/download/14.1.1/"
     "ripgrep-14.1.1-aarch64-unknown-linux-gnu.tar.gz",
     "c827481c4ff4ea10c9dc7a4022c8de5db34a5737cb74484d62eb94a95841ab2f",
     BinaryArchiveFormat::kTarGz,
     "ripgrep-14.1.1-aarch64-unknown-linux-gnu/rg"},
    {HostPlatform::kMac, HostArch::kX64,
     "https://github.com/BurntSushi/ripgrep/releases/download/14.1.1/"
     "ripgrep-14.1.1-x86_64-apple-darwin.tar.gz",
     "fc87e78f7cb3fea12d69072e7ef3b21509754717b746368fd40d88963630e2b3",
     BinaryArchiveFormat::kTarGz, "ripgrep-14.1.1-x86_64-apple-darwin/rg"},
    {HostPlatform::kMac, HostArch::kArm64,
     "https://github.com/BurntSushi/ripgrep/releases/download/14.1.1/"
     "ripgrep-14.1.1-aarch64-apple-darwin.tar.gz",
     "24ad76777745fbff131c8fbc466742b011f925bfa4fffa2ded6def23b5b937be",
     BinaryArchiveFormat::kTarGz, "ripgrep-14.1.1-aarch64-apple-darwin/rg"},
}};

constexpr std::array<BinarySpec, 1> kRegistry = {{
    {"rg", "ripgrep", "14.1.1"},
}};

// The per-target assets for a binary id. Kept as a by-value span return
// (not a struct member) so the chromium-rawptr checker has nothing to
// flag — the underlying arrays are constexpr static storage.
base::span<const BinaryAsset> AssetsForId(std::string_view id) {
  if (id == "rg") {
    return base::span<const BinaryAsset>(kRipgrepAssets);
  }
  return {};
}

}  // namespace

ResolvedBinaryAsset::ResolvedBinaryAsset() = default;
ResolvedBinaryAsset::ResolvedBinaryAsset(std::string url,
                                         std::string sha256_hex,
                                         BinaryArchiveFormat format,
                                         std::string member)
    : url(std::move(url)),
      sha256_hex(std::move(sha256_hex)),
      format(format),
      member(std::move(member)) {}
ResolvedBinaryAsset::ResolvedBinaryAsset(const ResolvedBinaryAsset&) = default;
ResolvedBinaryAsset::ResolvedBinaryAsset(ResolvedBinaryAsset&&) = default;
ResolvedBinaryAsset& ResolvedBinaryAsset::operator=(const ResolvedBinaryAsset&) =
    default;
ResolvedBinaryAsset& ResolvedBinaryAsset::operator=(ResolvedBinaryAsset&&) =
    default;
ResolvedBinaryAsset::~ResolvedBinaryAsset() = default;

HostPlatform CurrentHostPlatform() {
#if BUILDFLAG(IS_WIN)
  return HostPlatform::kWindows;
#elif BUILDFLAG(IS_MAC)
  return HostPlatform::kMac;
#else
  return HostPlatform::kLinux;
#endif
}

HostArch CurrentHostArch() {
#if defined(ARCH_CPU_ARM64)
  return HostArch::kArm64;
#else
  return HostArch::kX64;
#endif
}

const BinarySpec* GetBinarySpec(std::string_view id) {
  for (const BinarySpec& spec : kRegistry) {
    if (spec.id == id) {
      return &spec;
    }
  }
  return nullptr;
}

const BinaryAsset* SelectBinaryAsset(std::string_view id,
                                     HostPlatform platform,
                                     HostArch arch) {
  for (const BinaryAsset& asset : AssetsForId(id)) {
    if (asset.platform == platform && asset.arch == arch) {
      return &asset;
    }
  }
  return nullptr;
}

const BinaryAsset* SelectBinaryAssetForHost(std::string_view id) {
  return SelectBinaryAsset(id, CurrentHostPlatform(), CurrentHostArch());
}

std::optional<ResolvedBinaryAsset> ResolveBinaryAssetForHost(
    std::string_view id) {
  const HostPlatform platform = CurrentHostPlatform();
  const HostArch arch = CurrentHostArch();

  // The online-published config wins, so the pinned set can be updated
  // without rebuilding the host.
  std::optional<ResolvedBinaryAsset> published =
      NexusBinaryConfigRegistry::Get().Resolve(id, platform, arch);
  if (published.has_value()) {
    return published;
  }

  // Compiled-in fallback (offline / nothing published yet).
  const BinaryAsset* asset = SelectBinaryAsset(id, platform, arch);
  if (!asset) {
    return std::nullopt;
  }
  return ResolvedBinaryAsset{std::string(asset->url),
                             std::string(asset->sha256_hex), asset->format,
                             std::string(asset->member)};
}

}  // namespace nexus
