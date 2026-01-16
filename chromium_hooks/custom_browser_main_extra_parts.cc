#include "custom_browser/chromium_hooks/custom_browser_main_extra_parts.h"

#include <mutex>

#include "custom_browser/chromium_hooks/hook_dispatcher.h"
#include "custom_browser/features/hooks_registration.h"

namespace custom_browser {

namespace {

void RegisterHooksOnce() {
  static std::once_flag once_flag;
  std::call_once(once_flag, []() {
    custom_browser::features::RegisterAllHooks(GetHookRegistrar());
  });
}

}  // namespace

CustomBrowserMainExtraParts::CustomBrowserMainExtraParts() {
  RegisterHooksOnce();
}

CustomBrowserMainExtraParts::~CustomBrowserMainExtraParts() = default;

void CustomBrowserMainExtraParts::PreCreateThreads() {
  RunBrowserMainPreCreateThreadsHooks();
}

void CustomBrowserMainExtraParts::PostCreateThreads() {
  RunBrowserMainPostCreateThreadsHooks();
}

void CustomBrowserMainExtraParts::PreMainMessageLoopRun() {
  RunBrowserMainPreMainMessageLoopRunHooks();
}

void CustomBrowserMainExtraParts::PostMainMessageLoopRun() {
  RunBrowserMainPostMainMessageLoopRunHooks();
}

}  // namespace custom_browser
