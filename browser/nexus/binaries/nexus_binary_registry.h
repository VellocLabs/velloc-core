// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_BINARIES_NEXUS_BINARY_REGISTRY_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_BINARIES_NEXUS_BINARY_REGISTRY_H_

#include <optional>
#include <string>
#include <string_view>

namespace nexus {

// Container format of a downloaded release archive.
enum class BinaryArchiveFormat {
  kZip,
  kTarGz,
};

enum class HostPlatform {
  kWindows,
  kMac,
  kLinux,
};

enum class HostArch {
  kX64,
  kArm64,
};

// One pinned release asset for one (binary, platform, arch). The URL and
// SHA-256 are PINNED — never model/user-supplied — which is what makes
// "download then execute" safe: the bytes are verified against
// `sha256_hex` before anything is written to disk or run.
struct BinaryAsset {
  HostPlatform platform;
  HostArch arch;
  // Pinned download URL (a single GitHub release asset; follows the 302
  // to the release CDN).
  std::string_view url;
  // Lowercase hex SHA-256 of the archive bytes.
  std::string_view sha256_hex;
  BinaryArchiveFormat format;
  // Path of the executable member INSIDE the archive (forward slashes).
  std::string_view member;
};

// Metadata for a managed binary: an id (== shell tool id == on-disk
// basename, e.g. "rg") plus a label/version for logs. The per-target
// assets are looked up separately (see SelectBinaryAsset) so this struct
// holds no pointer/span members.
struct BinarySpec {
  std::string_view id;
  std::string_view label;
  std::string_view version;
};

// A resolved asset with OWNED strings. The compiled registry stores
// string_views into static data (BinaryAsset); the runtime-published
// config (NexusBinaryConfigRegistry) owns its strings. ResolvedBinaryAsset
// is the common value type the installer consumes regardless of source.
// Out-of-line special members (chromium-style: a header struct with
// non-trivial members needs them).
struct ResolvedBinaryAsset {
  ResolvedBinaryAsset();
  ResolvedBinaryAsset(std::string url,
                      std::string sha256_hex,
                      BinaryArchiveFormat format,
                      std::string member);
  ResolvedBinaryAsset(const ResolvedBinaryAsset&);
  ResolvedBinaryAsset(ResolvedBinaryAsset&&);
  ResolvedBinaryAsset& operator=(const ResolvedBinaryAsset&);
  ResolvedBinaryAsset& operator=(ResolvedBinaryAsset&&);
  ~ResolvedBinaryAsset();

  std::string url;
  std::string sha256_hex;
  BinaryArchiveFormat format = BinaryArchiveFormat::kZip;
  std::string member;
};

// The host platform/arch this browser was built for. Using the compile-
// time target (not runtime detection) guarantees the downloaded binary's
// arch matches a child process the browser can actually launch.
HostPlatform CurrentHostPlatform();
HostArch CurrentHostArch();

// Look up a managed binary's metadata by id, or nullptr when unknown.
const BinarySpec* GetBinarySpec(std::string_view id);

// Select the asset matching (platform, arch) for binary `id`, or nullptr
// when the id is unknown or the target is unsupported.
const BinaryAsset* SelectBinaryAsset(std::string_view id,
                                     HostPlatform platform,
                                     HostArch arch);

// Convenience: the compiled-in asset for `id` on the current host, or
// nullptr.
const BinaryAsset* SelectBinaryAssetForHost(std::string_view id);

// Resolve the asset for `id` on the current host as an owned value,
// preferring the runtime-published config (NexusBinaryConfigRegistry) and
// falling back to the compiled-in table. nullopt when neither has it.
// This is what NexusBinaryInstaller uses.
std::optional<ResolvedBinaryAsset> ResolveBinaryAssetForHost(
    std::string_view id);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_BINARIES_NEXUS_BINARY_REGISTRY_H_
