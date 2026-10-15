// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/features/patch_webui/custom_browser_strings_hook.h"

#include <utility>

#include "base/files/file_util.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"

namespace custom_browser::patch_webui {

namespace {

constexpr char kFallbackLocale[] = "en-US";

base::FilePath ExistingPakFor(const base::FilePath& assets_dir,
                              std::string_view locale) {
  if (locale.empty()) {
    return base::FilePath();
  }
  const base::FilePath pak_path = assets_dir.AppendASCII(
      base::StrCat({"custom_browser_strings_", locale, ".pak"}));
  return base::PathExists(pak_path) ? pak_path : base::FilePath();
}

}  // namespace

base::FilePath SelectCustomBrowserStringsPak(const base::FilePath& assets_dir,
                                             std::string_view app_locale) {
  std::string locale(app_locale);
  base::ReplaceChars(locale, "_", "-", &locale);

  if (base::FilePath pak = ExistingPakFor(assets_dir, locale); !pak.empty()) {
    return pak;
  }
  const size_t dash = locale.find('-');
  if (dash != std::string::npos) {
    if (base::FilePath pak =
            ExistingPakFor(assets_dir, std::string_view(locale).substr(0, dash));
        !pak.empty()) {
      return pak;
    }
  }
  return ExistingPakFor(assets_dir, kFallbackLocale);
}

CustomBrowserStringsHook::CustomBrowserStringsHook(base::FilePath assets_dir,
                                                   LocaleSource locale_source,
                                                   PakLoader pak_loader)
    : assets_dir_(std::move(assets_dir)),
      locale_source_(std::move(locale_source)),
      pak_loader_(std::move(pak_loader)) {}

CustomBrowserStringsHook::~CustomBrowserStringsHook() = default;

void CustomBrowserStringsHook::PreCreateThreads() {
  const base::FilePath pak =
      SelectCustomBrowserStringsPak(assets_dir_, locale_source_.Run());
  if (!pak.empty()) {
    pak_loader_.Run(pak);
  }
}

}  // namespace custom_browser::patch_webui
