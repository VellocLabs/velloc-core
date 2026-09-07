#ifndef CUSTOM_BROWSER_UI_WEB_UI_CUSTOM_BROWSER_WEBUI_H_
#define CUSTOM_BROWSER_UI_WEB_UI_CUSTOM_BROWSER_WEBUI_H_

#include "base/memory/raw_ptr.h"
#include "chrome/browser/ui/thumbnails/thumbnail_tracker.h"
#include "chrome/browser/ui/webui/webui_load_timer.h"
#include "chrome/common/webui_url_constants.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_ui_controller.h"
#include "content/public/browser/webui_config.h"
#include "content/public/common/url_constants.h"
#include "custom_browser/ui/mojom/browser_controller.mojom.h"
#include "custom_browser/ui/mojom/ui_view_controller.mojom.h"
#include "custom_browser/ui/web_ui/custom_browser_constants.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "ui/webui/mojo_web_ui_controller.h"
#include "ui/webui/resources/cr_components/color_change_listener/color_change_listener.mojom.h"


class Browser;
class PrefService;

class CustomBrowserHandler;
class UIViewHandler;
class CustomBrowserView;

class CustomBrowserWebUI;

class CustomBrowserWebUIConfig
    : public content::DefaultWebUIConfig<CustomBrowserWebUI> {
 public:
  CustomBrowserWebUIConfig()
      : DefaultWebUIConfig(content::kChromeUIScheme,
                           chrome::kChromeUICustomBrowserHost) {}
};

// The WebUI version of the tab strip in the browser. It is currently only
// supported on ChromeOS in tablet mode.
class CustomBrowserWebUI : public ui::MojoWebUIController,
                           public custom_browser::mojom::PageHandlerFactory {
 public:
  explicit CustomBrowserWebUI(content::WebUI* web_ui);

  CustomBrowserWebUI(const CustomBrowserWebUI&) = delete;
  CustomBrowserWebUI& operator=(const CustomBrowserWebUI&) = delete;

  ~CustomBrowserWebUI() override;

  // Instantiates the implementor of the mojom::PageHandlerFactory mojo
  // interface passing the pending receiver that will be internally bound.
  void BindInterface(
      mojo::PendingReceiver<custom_browser::mojom::PageHandlerFactory>
          receiver);

  // Instantiates the implementor of the mojom::BrowserHandler mojo interface
  // passing the pending receiver that will be internally bound.
  void BindInterface(
      mojo::PendingReceiver<color_change_listener::mojom::PageHandler>
          receiver);

  //   // Instantiates the implementor of the mojom::TabStripService mojo
  //   // interface passing the pending receiver that will be internally bound.
  //   void BindInterface(
  //       mojo::PendingReceiver<tabs_api::mojom::TabStripService> receiver);

  // Initialize CustomBrowserWebUI with its Browser it's running in.
  // Must be called exactly once. The WebUI won't work until this is called.
  // |Deinitialize| is called during |embedder|'s destructor. It release the
  // references taken previously and release the objects depending on them.
  void Initialize(CustomBrowserView* browser);
  void Deinitialize();

  // Background-panel hand-over (see nexus::NexusBackgroundPanelHost). The
  // page keeps living across a window close: DetachFromView() unhooks the
  // page handlers from the dying window (they stop observing its tab strip /
  // ui prefs and pause their Mojo receivers, so the page's remotes stay
  // bound) and AttachToView() re-points them at the next kNexus window,
  // replays that window's tabs / mode to the page and resumes the receivers.
  // A page that never created its handlers (CreatePageHandler not reached
  // yet) simply gets the new view for when it does.
  void DetachFromView();
  void AttachToView(CustomBrowserView* browser);

  // The embedder should call this whenever the result of
  // Embedder::GetLayout() changes.
  void LayoutChanged();

  // The embedder should call this whenever the tab strip gains keyboard focus.
  void ReceivedKeyboardFocus();

 private:
  // custom_browser::mojom::PageHandlerFactory
  void CreatePageHandler(
      mojo::PendingRemote<custom_browser::mojom::Browser> browser,
      mojo::PendingRemote<custom_browser::mojom::UIView> native_view,
      mojo::PendingReceiver<custom_browser::mojom::BrowserHandler>
          browser_handler_receiver,
      mojo::PendingReceiver<custom_browser::mojom::UIViewHandler>
          native_view_handler_receiver) override;

  WebuiLoadTimer webui_load_timer_;

  std::unique_ptr<CustomBrowserHandler> custom_browser_handler_;
  std::unique_ptr<UIViewHandler> native_view_handler_;
  // std::unique_ptr<CustomBrowserHandler> page_handler_;

  mojo::Receiver<custom_browser::mojom::PageHandlerFactory>
      page_factory_receiver_{this};

  raw_ptr<CustomBrowserView> custom_browser_view_ = nullptr;

  // A CreatePageHandler that arrived while no window was attached (the page
  // was parked before its renderer bound its handlers). Held until
  // AttachToView, which binds it against the new window — otherwise the
  // page's remotes would dangle forever, since the renderer asks only once.
  struct PendingPageHandler {
    PendingPageHandler();
    ~PendingPageHandler();
    mojo::PendingRemote<custom_browser::mojom::Browser> browser;
    mojo::PendingRemote<custom_browser::mojom::UIView> native_view;
    mojo::PendingReceiver<custom_browser::mojom::BrowserHandler>
        browser_handler_receiver;
    mojo::PendingReceiver<custom_browser::mojom::UIViewHandler>
        native_view_handler_receiver;
  };
  std::unique_ptr<PendingPageHandler> pending_page_handler_;

  WEB_UI_CONTROLLER_TYPE_DECL();
};

#endif  // CUSTOM_BROWSER_UI_WEB_UI_CUSTOM_BROWSER_WEBUI_H_
