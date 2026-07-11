// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/binaries/nexus_binary_registry.h"

#include "base/strings/string_util.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

TEST(NexusBinaryRegistryTest, RipgrepResolvesEverySupportedTarget) {
  const BinarySpec* spec = GetBinarySpec("rg");
  ASSERT_TRUE(spec);
  EXPECT_EQ(spec->id, "rg");
  EXPECT_EQ(spec->label, "ripgrep");

  struct Case {
    HostPlatform platform;
    HostArch arch;
    BinaryArchiveFormat format;
    const char* member_suffix;
  };
  const Case cases[] = {
      {HostPlatform::kWindows, HostArch::kX64, BinaryArchiveFormat::kZip,
       "/rg.exe"},
      {HostPlatform::kWindows, HostArch::kArm64, BinaryArchiveFormat::kZip,
       "/rg.exe"},
      {HostPlatform::kLinux, HostArch::kX64, BinaryArchiveFormat::kTarGz,
       "/rg"},
      {HostPlatform::kLinux, HostArch::kArm64, BinaryArchiveFormat::kTarGz,
       "/rg"},
      {HostPlatform::kMac, HostArch::kX64, BinaryArchiveFormat::kTarGz, "/rg"},
      {HostPlatform::kMac, HostArch::kArm64, BinaryArchiveFormat::kTarGz,
       "/rg"},
  };

  for (const Case& c : cases) {
    const BinaryAsset* asset = SelectBinaryAsset("rg", c.platform, c.arch);
    ASSERT_TRUE(asset);
    EXPECT_EQ(asset->format, c.format);
    EXPECT_TRUE(base::EndsWith(asset->member, c.member_suffix));
    // A pinned, well-formed 64-hex-char SHA-256 and an https URL.
    EXPECT_EQ(asset->sha256_hex.size(), 64u);
    EXPECT_TRUE(base::StartsWith(asset->url, "https://"));
  }
}

TEST(NexusBinaryRegistryTest, UnknownIdsReturnNull) {
  EXPECT_FALSE(GetBinarySpec("does-not-exist"));
  EXPECT_FALSE(SelectBinaryAssetForHost("does-not-exist"));
  EXPECT_FALSE(GetBinarySpec(""));
}

TEST(NexusBinaryRegistryTest, CurrentHostHasARipgrepAsset) {
  // Whatever platform/arch this test binary was built for must be a
  // supported target — otherwise the feature silently no-ops on the host
  // it ships to.
  const BinaryAsset* asset = SelectBinaryAssetForHost("rg");
  ASSERT_TRUE(asset);
  EXPECT_EQ(asset->platform, CurrentHostPlatform());
  EXPECT_EQ(asset->arch, CurrentHostArch());
}

}  // namespace
}  // namespace nexus
