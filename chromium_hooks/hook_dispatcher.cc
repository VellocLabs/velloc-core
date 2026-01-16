#include "custom_browser/chromium_hooks/hook_dispatcher.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/no_destructor.h"
#include "custom_browser/chromium_hooks/hook_callbacks.h"
#include "custom_browser/chromium_hooks/hook_registrar.h"
#include "custom_browser/chromium_hooks/hook_types.h"

namespace custom_browser {

namespace {

struct WebUIDataSourceCallbackEntry {
  std::string host_path;
  WebUIDataSourceCreatedCallback callback;
};

class HookRegistryImpl final : public HookRegistrar {
 public:
  HookRegistryImpl() = default;
  ~HookRegistryImpl() override = default;

  void AddWebUIDataSourceCreatedCallback(
      std::string_view host_path,
      WebUIDataSourceCreatedCallback cb) override {
    webui_callbacks_.push_back({std::string(host_path), std::move(cb)});
  }

  void AddLocalizedStringOverrideCallback(
      LocalizedStringOverrideCallback cb) override {
    localized_string_callbacks_.push_back(std::move(cb));
  }

  void AddNavigationThrottleProvider(
      NavigationThrottleProviderCallback provider) override {
    nav_providers_.push_back(std::move(provider));
  }

  void AddBrowserMainLifecycleHook(
      std::unique_ptr<BrowserMainLifecycleHook> hook) override {
    main_hooks_.push_back(std::move(hook));
  }

  const std::vector<WebUIDataSourceCallbackEntry>& webui_callbacks() const {
    return webui_callbacks_;
  }
  const std::vector<LocalizedStringOverrideCallback>&
  localized_string_callbacks() const {
    return localized_string_callbacks_;
  }
  const std::vector<NavigationThrottleProviderCallback>& nav_providers() const {
    return nav_providers_;
  }
  const std::vector<std::unique_ptr<BrowserMainLifecycleHook>>& main_hooks()
      const {
    return main_hooks_;
  }

 private:
  std::vector<WebUIDataSourceCallbackEntry> webui_callbacks_;
  std::vector<LocalizedStringOverrideCallback> localized_string_callbacks_;
  std::vector<NavigationThrottleProviderCallback> nav_providers_;
  std::vector<std::unique_ptr<BrowserMainLifecycleHook>> main_hooks_;
};

HookRegistryImpl& GetRegistry() {
  static base::NoDestructor<HookRegistryImpl> registry;
  return *registry;
}

}  // namespace

HookRegistrar* GetHookRegistrar() {
  return &GetRegistry();
}

void OnWebUIDataSourceCreated(content::BrowserContext* browser_context,
                              std::string_view host,
                              content::WebUIDataSource* data_source) {
  for (const auto& entry : GetRegistry().webui_callbacks()) {
    if (entry.host_path != host) {
      continue;
    }
    entry.callback.Run(browser_context, data_source);
  }
}

void NotifyWebUIDataSourceCreated(content::BrowserContext* browser_context,
                                  std::string_view host,
                                  content::WebUIDataSource* data_source) {
  OnWebUIDataSourceCreated(browser_context, host, data_source);
}

int OnAddLocalizedString(content::WebUIDataSource* data_source,
                         std::string_view name,
                         int ids) {
  int resolved_ids = ids;
  for (const auto& cb : GetRegistry().localized_string_callbacks()) {
    resolved_ids = cb.Run(data_source, name, resolved_ids);
  }
  return resolved_ids;
}

void AppendNavigationThrottles(
    content::NavigationThrottleRegistry& registry) {
  for (const auto& provider : GetRegistry().nav_providers()) {
    provider.Run(registry);
  }
}

void RunBrowserMainPreCreateThreadsHooks() {
  for (const auto& hook : GetRegistry().main_hooks()) {
    hook->PreCreateThreads();
  }
}

void RunBrowserMainPostCreateThreadsHooks() {
  for (const auto& hook : GetRegistry().main_hooks()) {
    hook->PostCreateThreads();
  }
}

void RunBrowserMainPreMainMessageLoopRunHooks() {
  for (const auto& hook : GetRegistry().main_hooks()) {
    hook->PreMainMessageLoopRun();
  }
}

void RunBrowserMainPostMainMessageLoopRunHooks() {
  for (const auto& hook : GetRegistry().main_hooks()) {
    hook->PostMainMessageLoopRun();
  }
}

}  // namespace custom_browser
