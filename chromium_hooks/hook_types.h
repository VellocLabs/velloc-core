#ifndef CUSTOM_BROWSER_CHROMIUM_HOOKS_HOOK_TYPES_H_
#define CUSTOM_BROWSER_CHROMIUM_HOOKS_HOOK_TYPES_H_

namespace custom_browser {

// Multi-stage lifecycle hook for browser main.
class BrowserMainLifecycleHook {
 public:
  virtual ~BrowserMainLifecycleHook() = default;

  virtual void PreCreateThreads() {}
  virtual void PostCreateThreads() {}
  virtual void PreMainMessageLoopRun() {}
  virtual void PostMainMessageLoopRun() {}
};

}  // namespace custom_browser

#endif  // CUSTOM_BROWSER_CHROMIUM_HOOKS_HOOK_TYPES_H_
