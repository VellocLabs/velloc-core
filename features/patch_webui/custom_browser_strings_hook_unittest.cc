// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/features/patch_webui/custom_browser_strings_hook.h"

#include <string>
#include <vector>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/strings/strcat.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace custom_browser::patch_webui {
namespace {

class CustomBrowserStringsHookTest : public testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(assets_dir_.CreateUniqueTempDir()); }

  base::FilePath AddPak(const std::string& locale) {
    const base::FilePath path = PakPath(locale);
    EXPECT_TRUE(base::WriteFile(path, ""));
    return path;
  }

  base::FilePath PakPath(const std::string& locale) const {
    return assets_dir_.GetPath().AppendASCII(
        base::StrCat({"custom_browser_strings_", locale, ".pak"}));
  }

  const base::FilePath& assets_dir() const { return assets_dir_.GetPath(); }

 private:
  base::ScopedTempDir assets_dir_;
};

TEST_F(CustomBrowserStringsHookTest, SelectsExactLocale) {
  AddPak("en-US");
  const base::FilePath zh_cn = AddPak("zh-CN");
  EXPECT_EQ(zh_cn, SelectCustomBrowserStringsPak(assets_dir(), "zh-CN"));
  EXPECT_EQ(zh_cn, SelectCustomBrowserStringsPak(assets_dir(), "zh_CN"));
}

TEST_F(CustomBrowserStringsHookTest, FallsBackToLanguageThenEnUs) {
  const base::FilePath en_us = AddPak("en-US");
  const base::FilePath pt = AddPak("pt");
  EXPECT_EQ(pt, SelectCustomBrowserStringsPak(assets_dir(), "pt-BR"));
  EXPECT_EQ(en_us, SelectCustomBrowserStringsPak(assets_dir(), "sw"));
  EXPECT_EQ(en_us, SelectCustomBrowserStringsPak(assets_dir(), ""));
}

TEST_F(CustomBrowserStringsHookTest, NoPakSelectsNothing) {
  EXPECT_TRUE(SelectCustomBrowserStringsPak(assets_dir(), "zh-CN").empty());
}

// The hook is built from RegisterHooks(), before BrowserProcess exists and the
// application locale is known; the pak must follow the locale as it stands at
// PreCreateThreads, not at construction.
TEST(CustomBrowserStringsPakLocaleTest, ReadsTheLocaleFromThePakName) {
  EXPECT_EQ("zh-CN", CustomBrowserStringsPakLocale(base::FilePath(
                         FILE_PATH_LITERAL("a/custom_browser_strings_zh-CN.pak"))));
  EXPECT_EQ("en-US", CustomBrowserStringsPakLocale(base::FilePath(
                         FILE_PATH_LITERAL("custom_browser_strings_en-US.pak"))));
  EXPECT_EQ("", CustomBrowserStringsPakLocale(base::FilePath(
                    FILE_PATH_LITERAL("custom_browser_strings_.pak"))));
  EXPECT_EQ("", CustomBrowserStringsPakLocale(
                    base::FilePath(FILE_PATH_LITERAL("resources.pak"))));
}

TEST_F(CustomBrowserStringsHookTest, ReadsTheLocaleAtPreCreateThreads) {
  AddPak("en-US");
  const base::FilePath zh_cn = AddPak("zh-CN");

  std::string app_locale;
  std::vector<base::FilePath> loaded;
  CustomBrowserStringsHook hook(
      assets_dir(),
      base::BindRepeating([](const std::string* locale) { return *locale; },
                          base::Unretained(&app_locale)),
      base::BindRepeating(
          [](std::vector<base::FilePath>* out, const base::FilePath& path) {
            out->push_back(path);
          },
          base::Unretained(&loaded)));
  EXPECT_TRUE(loaded.empty());

  app_locale = "zh-CN";
  hook.PreCreateThreads();

  ASSERT_EQ(1u, loaded.size());
  EXPECT_EQ(zh_cn, loaded[0]);
}

}  // namespace
}  // namespace custom_browser::patch_webui
