#ifndef CUSTOM_BROWSER_CHROMIUM_HOOKS_CUSTOM_BROWSER_MAIN_EXTRA_PARTS_H_
#define CUSTOM_BROWSER_CHROMIUM_HOOKS_CUSTOM_BROWSER_MAIN_EXTRA_PARTS_H_

#include "chrome/browser/chrome_browser_main_extra_parts.h"

namespace custom_browser {

// Integration entry point for browser main lifecycle hooks.
class CustomBrowserMainExtraParts : public ChromeBrowserMainExtraParts {
 public:
  CustomBrowserMainExtraParts();
  ~CustomBrowserMainExtraParts() override;

  void PreCreateThreads() override;
  void PostCreateThreads() override;
  void PreMainMessageLoopRun() override;
  void PostMainMessageLoopRun() override;
};

}  // namespace custom_browser

#endif  // CUSTOM_BROWSER_CHROMIUM_HOOKS_CUSTOM_BROWSER_MAIN_EXTRA_PARTS_H_
