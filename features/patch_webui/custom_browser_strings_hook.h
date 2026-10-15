// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_FEATURES_PATCH_WEBUI_CUSTOM_BROWSER_STRINGS_HOOK_H_
#define CUSTOM_BROWSER_FEATURES_PATCH_WEBUI_CUSTOM_BROWSER_STRINGS_HOOK_H_

#include <string>
#include <string_view>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "custom_browser/chromium_hooks/hook_types.h"

namespace custom_browser::patch_webui {

// Returns the custom_browser_strings pak in |assets_dir| for |app_locale|
// ("zh-CN" or "zh_CN"): the exact locale, then its language ("pt-BR" -> "pt"),
// then en-US. Returns an empty path when none of them exists.
base::FilePath SelectCustomBrowserStringsPak(const base::FilePath& assets_dir,
                                             std::string_view app_locale);

// Loads the custom_browser_strings pak for the application locale at
// PreCreateThreads. Constructing it must stay inert: it is built from
// RegisterHooks(), before BrowserProcess exists and the locale is known.
class CustomBrowserStringsHook : public BrowserMainLifecycleHook {
 public:
  using LocaleSource = base::RepeatingCallback<std::string()>;
  using PakLoader = base::RepeatingCallback<void(const base::FilePath&)>;

  CustomBrowserStringsHook(base::FilePath assets_dir,
                           LocaleSource locale_source,
                           PakLoader pak_loader);
  CustomBrowserStringsHook(const CustomBrowserStringsHook&) = delete;
  CustomBrowserStringsHook& operator=(const CustomBrowserStringsHook&) = delete;
  ~CustomBrowserStringsHook() override;

  // BrowserMainLifecycleHook:
  void PreCreateThreads() override;

 private:
  const base::FilePath assets_dir_;
  const LocaleSource locale_source_;
  const PakLoader pak_loader_;
};

}  // namespace custom_browser::patch_webui

#endif  // CUSTOM_BROWSER_FEATURES_PATCH_WEBUI_CUSTOM_BROWSER_STRINGS_HOOK_H_
