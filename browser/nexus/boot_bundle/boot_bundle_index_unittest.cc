// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/boot_bundle/boot_bundle_index.h"

#include <optional>

#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace nexus {
namespace {

constexpr char kIndex[] = R"({
  "format": 1,
  "buildTimeMs": 1758774728561,
  "base": "/v1/",
  "origins": ["https://app.velloc.app", "https://beta.velloc.app"],
  "files": [
    {"path": "index.html", "offset": 0, "size": 10, "type": "text/html"},
    {"path": "assets/main-abc123.js", "offset": 10, "size": 5,
     "type": "text/javascript"}
  ]
})";

TEST(BootBundleIndexTest, DirectoryUrlMapsToIndexHtml) {
  std::optional<BootBundleIndex> index = BootBundleIndex::Parse(kIndex, 15);
  ASSERT_TRUE(index);
  const BootBundleEntry* entry =
      index->Lookup(GURL("https://app.velloc.app/v1/?channel=beta"));
  ASSERT_TRUE(entry);
  EXPECT_EQ(entry->mime_type, "text/html");
  EXPECT_EQ(entry->size, 10u);
}

TEST(BootBundleIndexTest, ServesEveryListedOrigin) {
  std::optional<BootBundleIndex> index = BootBundleIndex::Parse(kIndex, 15);
  ASSERT_TRUE(index);
  const BootBundleEntry* entry =
      index->Lookup(GURL("https://beta.velloc.app/v1/assets/main-abc123.js"));
  ASSERT_TRUE(entry);
  EXPECT_EQ(entry->offset, 10u);
  EXPECT_EQ(index->build_time().InMillisecondsSinceUnixEpoch(),
            1758774728561);
}

TEST(BootBundleIndexTest, RejectsUrlsOutsideTheBundle) {
  std::optional<BootBundleIndex> index = BootBundleIndex::Parse(kIndex, 15);
  ASSERT_TRUE(index);
  EXPECT_FALSE(index->Lookup(GURL("http://app.velloc.app/v1/")));
  EXPECT_FALSE(index->Lookup(GURL("https://example.com/v1/")));
  EXPECT_FALSE(index->Lookup(GURL("https://app.velloc.app/v2/")));
  EXPECT_FALSE(index->Lookup(GURL("https://app.velloc.app/")));
  EXPECT_FALSE(
      index->Lookup(GURL("https://app.velloc.app/v1/assets/other-1.js")));
  EXPECT_FALSE(index->Lookup(
      GURL("https://app.velloc.app/v1/../v1/assets/missing.js")));
}

TEST(BootBundleIndexTest, RejectsEntriesPastTheDataFile) {
  EXPECT_FALSE(BootBundleIndex::Parse(kIndex, 14));
}

TEST(BootBundleIndexTest, RejectsMalformedIndexes) {
  EXPECT_FALSE(BootBundleIndex::Parse("not json", 15));
  EXPECT_FALSE(BootBundleIndex::Parse(R"({"format": 2})", 15));
  EXPECT_FALSE(BootBundleIndex::Parse(
      R"({"format":1,"buildTimeMs":1,"base":"v1","origins":[],"files":[]})",
      0));
  EXPECT_FALSE(BootBundleIndex::Parse(
      R"({"format":1,"buildTimeMs":1,"base":"/v1/",
          "origins":["http://app.velloc.app"],"files":[]})",
      0));
}

TEST(BootBundleIndexTest, EmptyFileListParsesAsEmpty) {
  std::optional<BootBundleIndex> index = BootBundleIndex::Parse(
      R"({"format":1,"buildTimeMs":1,"base":"/v1/",
          "origins":["https://app.velloc.app"],"files":[]})",
      0);
  ASSERT_TRUE(index);
  EXPECT_TRUE(index->empty());
}

}  // namespace
}  // namespace nexus
