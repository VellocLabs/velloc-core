#include "custom_browser/chromium_hooks/hook_dispatcher.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/no_destructor.h"
#include "content/public/browser/web_ui_data_source.h"
#include "custom_browser/chromium_hooks/hook_callbacks.h"
#include "custom_browser/chromium_hooks/hook_registrar.h"
#include "custom_browser/chromium_hooks/hook_types.h"
#include "mojo/public/cpp/bindings/generic_pending_receiver.h"

namespace custom_browser {

namespace {

struct WebUIDataSourceConfiguringCallbackEntry {
  std::string host_path;
  WebUIDataSourceConfiguringCallback callback;
};

struct RenderFrameInterfaceBinderEntry {
  std::string interface_name;
  RenderFrameInterfaceBinderCallback callback;
};

class HookRegistryImpl final : public HookRegistrar {
 public:
  HookRegistryImpl() = default;
  ~HookRegistryImpl() override = default;

  void AddWebUIDataSourceConfiguringCallback(
      std::string_view host_path,
      WebUIDataSourceConfiguringCallback cb) override {
    configuring_callbacks_.push_back({std::string(host_path), std::move(cb)});
  }

  void AddNavigationThrottleProvider(
      NavigationThrottleProviderCallback provider) override {
    nav_providers_.push_back(std::move(provider));
  }

  void AddWebUIRenderFrameBinderMapPopulator(
      WebUIRenderFrameBinderMapPopulatorCallback cb) override {
    webui_render_frame_binder_map_populators_.push_back(std::move(cb));
  }

  void AddRenderFrameInterfaceBinder(
      std::string_view interface_name,
      RenderFrameInterfaceBinderCallback cb) override {
    render_frame_interface_binders_.push_back(
        {std::string(interface_name), std::move(cb)});
  }

  void AddBrowserMainLifecycleHook(
      std::unique_ptr<BrowserMainLifecycleHook> hook) override {
    main_hooks_.push_back(std::move(hook));
  }

  void SetBubbleAnchorRectOverride(
      BubbleAnchorRectOverrideCallback cb) override {
    bubble_anchor_rect_override_ = std::move(cb);
  }

  const std::vector<WebUIDataSourceConfiguringCallbackEntry>& configuring_callbacks() const {
    return configuring_callbacks_;
  }
  const std::vector<NavigationThrottleProviderCallback>& nav_providers() const {
    return nav_providers_;
  }
  const std::vector<WebUIRenderFrameBinderMapPopulatorCallback>&
  webui_render_frame_binder_map_populators() const {
    return webui_render_frame_binder_map_populators_;
  }
  const std::vector<RenderFrameInterfaceBinderEntry>&
  render_frame_interface_binders() const {
    return render_frame_interface_binders_;
  }
  const std::vector<std::unique_ptr<BrowserMainLifecycleHook>>& main_hooks()
      const {
    return main_hooks_;
  }
  const BubbleAnchorRectOverrideCallback& bubble_anchor_rect_override() const {
    return bubble_anchor_rect_override_;
  }

 private:
  std::vector<WebUIDataSourceConfiguringCallbackEntry> configuring_callbacks_;
  std::vector<NavigationThrottleProviderCallback> nav_providers_;
  std::vector<WebUIRenderFrameBinderMapPopulatorCallback>
      webui_render_frame_binder_map_populators_;
  std::vector<RenderFrameInterfaceBinderEntry> render_frame_interface_binders_;
  std::vector<std::unique_ptr<BrowserMainLifecycleHook>> main_hooks_;
  BubbleAnchorRectOverrideCallback bubble_anchor_rect_override_;
};

HookRegistryImpl& GetRegistry() {
  static base::NoDestructor<HookRegistryImpl> registry;
  return *registry;
}

}  // namespace

HookRegistrar* GetHookRegistrar() {
  return &GetRegistry();
}

void OnWebUIConfiguringLocalResource(content::WebUI* web_ui,
                                     content::WebUIDataSource* data_source) {
  std::string host = data_source->GetSource();
  for (const auto& entry : GetRegistry().configuring_callbacks()) {
    if (entry.host_path != host) {
      continue;
    }
    entry.callback.Run(web_ui, data_source);
  }
}

void AppendNavigationThrottles(
    content::NavigationThrottleRegistry& registry) {
  for (const auto& provider : GetRegistry().nav_providers()) {
    provider.Run(registry);
  }
}

void PopulateWebUIRenderFrameBinders(
    mojo::BinderMapWithContext<content::RenderFrameHost*>* map) {
  if (!map) {
    return;
  }

  for (const auto& populator :
       GetRegistry().webui_render_frame_binder_map_populators()) {
    populator.Run(map);
  }
}

bool TryBindRenderFrameInterface(content::RenderFrameHost* render_frame_host,
                                 mojo::GenericPendingReceiver receiver) {
  if (!receiver.is_valid() || !receiver.interface_name()) {
    return false;
  }

  for (const auto& entry : GetRegistry().render_frame_interface_binders()) {
    if (entry.interface_name != *receiver.interface_name()) {
      continue;
    }
    if (entry.callback.Run(render_frame_host, &receiver)) {
      return true;
    }
  }

  return false;
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

std::optional<gfx::Rect> OnGetBubbleAnchorRect(
    const views::View* anchor_view,
    const gfx::Rect& default_rect) {
  const auto& cb = GetRegistry().bubble_anchor_rect_override();
  if (cb) {
    return cb.Run(anchor_view, default_rect);
  }
  return std::nullopt;
}

}  // namespace custom_browser
