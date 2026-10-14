// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/binaries/nexus_binary_registry.h"

#include <array>
#include <string>
#include <utility>

#include "base/containers/span.h"
#include "build/build_config.h"
#include "custom_browser/browser/nexus/binaries/nexus_binary_config_registry.h"

namespace nexus {

namespace {

// Compiled-in download pins: binary id -> label/version, plus one asset per
// supported platform/arch (URL, SHA-256 of the archive, archive format, and
// the executable's member path inside it). EMPTY today: ripgrep, the only
// entry, ships with the installer next to chrome.dll
// (custom_browser::GetBundledBinaryPath) and is never downloaded. The
// download machinery (NexusBinaryInstaller, the online-published
// NexusBinaryConfigRegistry) stays for the next binary that needs it; add
// its BinarySpec here and its assets in AssetsForId().
constexpr std::array<BinarySpec, 0> kRegistry = {};

// The per-target assets for a binary id. Kept as a by-value span return
// (not a struct member) so the chromium-rawptr checker has nothing to
// flag — the underlying arrays are constexpr static storage.
base::span<const BinaryAsset> AssetsForId(std::string_view id) {
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
