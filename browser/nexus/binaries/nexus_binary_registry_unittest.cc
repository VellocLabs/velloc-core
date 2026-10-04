// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/binaries/nexus_binary_registry.h"

#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

// ripgrep ships with the installer next to chrome.dll and is never
// downloaded: no compiled-in spec, and no asset on any target.
TEST(NexusBinaryRegistryTest, RipgrepIsNotDownloadable) {
  EXPECT_FALSE(GetBinarySpec("rg"));
  EXPECT_FALSE(SelectBinaryAssetForHost("rg"));

  const HostPlatform platforms[] = {HostPlatform::kWindows,
                                    HostPlatform::kLinux, HostPlatform::kMac};
  const HostArch arches[] = {HostArch::kX64, HostArch::kArm64};
  for (HostPlatform platform : platforms) {
    for (HostArch arch : arches) {
      EXPECT_FALSE(SelectBinaryAsset("rg", platform, arch));
    }
  }
}

TEST(NexusBinaryRegistryTest, UnknownIdsReturnNull) {
  EXPECT_FALSE(GetBinarySpec("does-not-exist"));
  EXPECT_FALSE(SelectBinaryAssetForHost("does-not-exist"));
  EXPECT_FALSE(GetBinarySpec(""));
}

}  // namespace
}  // namespace nexus
