// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/binaries/nexus_binary_config_registry.h"

#include "base/test/values_test_util.h"
#include "base/values.h"
#include "custom_browser/browser/nexus/binaries/nexus_binary_registry.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

base::DictValue ParseDict(const char* json) {
  return base::test::ParseJsonDict(json);
}

constexpr char kOneRgWindows[] = R"({
  "binaries": [{
    "id": "rg", "label": "ripgrep", "version": "9.9.9",
    "assets": [{
      "platform": "win32", "arch": "x64",
      "url": "https://example.test/rg.zip",
      "sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
      "format": "zip", "member": "pkg/rg.exe"
    }]
  }]
})";

class NexusBinaryConfigRegistryTest : public testing::Test {
 protected:
  void SetUp() override { NexusBinaryConfigRegistry::Get().ClearForTesting(); }
  void TearDown() override {
    NexusBinaryConfigRegistry::Get().ClearForTesting();
  }
};

TEST_F(NexusBinaryConfigRegistryTest, ParsesAndResolvesPublishedAsset) {
  NexusBinaryConfigRegistry& reg = NexusBinaryConfigRegistry::Get();
  EXPECT_TRUE(reg.empty());
  EXPECT_EQ(reg.SetFromDict(ParseDict(kOneRgWindows)), 1u);
  EXPECT_FALSE(reg.empty());

  std::optional<ResolvedBinaryAsset> a =
      reg.Resolve("rg", HostPlatform::kWindows, HostArch::kX64);
  ASSERT_TRUE(a.has_value());
  EXPECT_EQ(a->url, "https://example.test/rg.zip");
  EXPECT_EQ(a->format, BinaryArchiveFormat::kZip);
  EXPECT_EQ(a->member, "pkg/rg.exe");

  // A target the config doesn't list resolves to nothing here.
  EXPECT_FALSE(
      reg.Resolve("rg", HostPlatform::kLinux, HostArch::kX64).has_value());
  EXPECT_FALSE(
      reg.Resolve("nope", HostPlatform::kWindows, HostArch::kX64).has_value());
}

TEST_F(NexusBinaryConfigRegistryTest, SkipsMalformedAssetsButKeepsValidOnes) {
  // One asset is missing `sha256`, one has a non-hex sha, one is http (not
  // https), one is valid. Only the valid one is accepted.
  constexpr char kMixed[] = R"({
    "binaries": [{
      "id": "rg",
      "assets": [
        { "platform": "win32", "arch": "x64", "url": "https://x/y.zip",
          "format": "zip", "member": "rg.exe" },
        { "platform": "linux", "arch": "x64", "url": "https://x/y.tgz",
          "sha256": "nothex", "format": "tar.gz", "member": "rg" },
        { "platform": "darwin", "arch": "arm64", "url": "http://insecure/y.tgz",
          "sha256": "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
          "format": "tar.gz", "member": "rg" },
        { "platform": "linux", "arch": "arm64", "url": "https://x/ok.tgz",
          "sha256": "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
          "format": "tar.gz", "member": "pkg/rg" }
      ]
    }]
  })";
  NexusBinaryConfigRegistry& reg = NexusBinaryConfigRegistry::Get();
  EXPECT_EQ(reg.SetFromDict(ParseDict(kMixed)), 1u);
  EXPECT_TRUE(
      reg.Resolve("rg", HostPlatform::kLinux, HostArch::kArm64).has_value());
  EXPECT_FALSE(reg.Resolve("rg", HostPlatform::kWindows, HostArch::kX64)
                   .has_value());  // missing sha
  EXPECT_FALSE(reg.Resolve("rg", HostPlatform::kMac, HostArch::kArm64)
                   .has_value());  // http
}

TEST_F(NexusBinaryConfigRegistryTest, AllowsLoopbackHttpButNotRemoteHttp) {
  constexpr char kJson[] = R"({
    "binaries": [{ "id": "rg", "assets": [
      { "platform":"linux","arch":"x64","url":"http://127.0.0.1:8080/rg.tgz",
        "sha256":"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
        "format":"tar.gz","member":"pkg/rg" },
      { "platform":"linux","arch":"arm64","url":"http://evil.example/rg.tgz",
        "sha256":"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd",
        "format":"tar.gz","member":"pkg/rg" }
    ]}]
  })";
  NexusBinaryConfigRegistry& reg = NexusBinaryConfigRegistry::Get();
  EXPECT_EQ(reg.SetFromDict(ParseDict(kJson)), 1u);
  EXPECT_TRUE(
      reg.Resolve("rg", HostPlatform::kLinux, HostArch::kX64).has_value());
  EXPECT_FALSE(
      reg.Resolve("rg", HostPlatform::kLinux, HostArch::kArm64).has_value());
}

TEST_F(NexusBinaryConfigRegistryTest, EmptyOrInvalidPublishKeepsPreviousConfig) {
  NexusBinaryConfigRegistry& reg = NexusBinaryConfigRegistry::Get();
  ASSERT_EQ(reg.SetFromDict(ParseDict(kOneRgWindows)), 1u);

  // A dict with no usable assets must NOT wipe the working config.
  EXPECT_EQ(reg.SetFromDict(ParseDict(R"({"binaries":[]})")), 0u);
  EXPECT_TRUE(
      reg.Resolve("rg", HostPlatform::kWindows, HostArch::kX64).has_value());

  EXPECT_EQ(reg.SetFromDict(ParseDict(R"({"unrelated":true})")), 0u);
  EXPECT_TRUE(
      reg.Resolve("rg", HostPlatform::kWindows, HostArch::kX64).has_value());
}

TEST_F(NexusBinaryConfigRegistryTest,
       ResolveBinaryAssetForHostUsesPublishedConfig) {
  // With nothing published there is nothing to download: rg ships with the
  // installer, so the compiled-in registry carries no asset for it.
  EXPECT_FALSE(ResolveBinaryAssetForHost("rg").has_value());

  // Publish an asset for THIS host's platform/arch; the download path stays
  // live, so the resolver must now return the published URL.
  base::DictValue dict;
  base::ListValue assets;
  base::DictValue asset;
  asset.Set("platform", CurrentHostPlatform() == HostPlatform::kWindows
                            ? "win32"
                            : CurrentHostPlatform() == HostPlatform::kMac
                                  ? "darwin"
                                  : "linux");
  asset.Set("arch",
            CurrentHostArch() == HostArch::kArm64 ? "arm64" : "x64");
  asset.Set("url", "https://override.test/custom-rg");
  asset.Set("sha256",
            "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd");
  asset.Set("format", CurrentHostPlatform() == HostPlatform::kWindows
                          ? "zip"
                          : "tar.gz");
  asset.Set("member", "custom/rg");
  assets.Append(std::move(asset));
  base::DictValue binary;
  binary.Set("id", "rg");
  binary.Set("assets", std::move(assets));
  base::ListValue binaries;
  binaries.Append(std::move(binary));
  dict.Set("binaries", std::move(binaries));

  ASSERT_EQ(NexusBinaryConfigRegistry::Get().SetFromDict(dict), 1u);
  std::optional<ResolvedBinaryAsset> overridden =
      ResolveBinaryAssetForHost("rg");
  ASSERT_TRUE(overridden.has_value());
  EXPECT_EQ(overridden->url, "https://override.test/custom-rg");
}

}  // namespace
}  // namespace nexus
