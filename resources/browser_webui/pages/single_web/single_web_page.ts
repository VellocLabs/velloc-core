import {
  CrLitElement,
  css,
  html,
  type CSSResultGroup,
} from 'chrome://resources/lit/v3_0/lit.rollup.js';
import {loadTimeData} from 'chrome://resources/js/load_time_data.js';

import {BrowserApiProxyImpl} from '../../browser_api_proxy.js';
import type {Tab} from '../../browser_controller.mojom-webui.js';
import '../../components/native/native_view_aliases.js';
import type {TabId} from '../../components/tabs_panel/types.js';
import {
  tabsController,
  type TabObserver,
  type TabsObserver,
} from '../../controller/tabs_controller.js';

/**
 * The address row's text for `url`: empty for a data: URL — the host's own
 * pages (the sign-in status pages) have no address worth showing.
 */
function shownAddress(url: string): string {
  return url.startsWith('data:') ? '' : url;
}

/**
 * Single-web window (kSingleWeb, F-29): a title row (product logo, the
 * window title, the OS caption buttons) over one tab with a read-only
 * address and the native back / forward / reload buttons. The title is the
 * creator's fixed title when it set one, else the page's own title. The
 * address is display only — navigation is the opener's (the `webWindow`
 * bridge capability) or the page's own.
 */
export class SingleWebPage extends CrLitElement {
  static get is() {
    return 'single-web-page';
  }

  static override get properties() {
    return {
      url_: {type: String, state: true},
      pageTitle_: {type: String, state: true},
      fixedTitle_: {type: String, state: true},
    };
  }

  protected accessor url_: string = '';
  protected accessor pageTitle_: string = '';
  protected accessor fixedTitle_: string = '';

  private fixedTitleListenerId_: number|null = null;

  private trackedTabId_: TabId|null = null;

  private readonly tabsObserver_: TabsObserver = {
    onTabsUpdated: (pinned, unpinned) => {
      this.trackTab_(tabsController.getActiveTabId() ?? unpinned[0] ??
                     pinned[0] ?? null);
    },
  };

  private readonly tabObserver_: TabObserver = {
    onTabDataUpdated: (tab: Tab) => {
      this.url_ = shownAddress(tab.url);
      this.pageTitle_ = tab.title;
    },
    onTabExtrasUpdated: () => {},
    onTabActiveStateChanged: () => {},
    onThumbnailChanged: () => {},
  };

  static override styles: CSSResultGroup = css`
    :host {
      display: flex;
      flex-direction: column;
      flex: 1 1 auto;
      width: 100%;
      height: 100%;
      min-height: 0;
      box-sizing: border-box;
      background: var(--color-toolbar);
      color: var(--text-primary);
    }

    #titlebar {
      display: flex;
      align-items: center;
      flex: 0 0 auto;
      height: 34px;
      min-width: 0;
      padding-inline-start: 12px;
      background: var(--color-frame-active);
      color: var(--color-frame-caption-foreground-active);
      -webkit-app-region: drag;
    }

    #logo {
      flex: 0 0 auto;
      width: 16px;
      height: 16px;
      margin-inline-end: 8px;
    }

    #title {
      flex: 1 1 auto;
      min-width: 0;
      font-size: 12px;
      line-height: 16px;
      white-space: nowrap;
      overflow: hidden;
      text-overflow: ellipsis;
    }

    #toolbar {
      display: flex;
      align-items: center;
      flex: 0 0 auto;
      height: 36px;
      min-width: 0;
      padding-inline: 4px;
      background: var(--color-toolbar);
      -webkit-app-region: drag;
    }

    single-web-navigation-view {
      flex: 0 0 auto;
      -webkit-app-region: no-drag;
    }

    #address {
      flex: 1 1 auto;
      min-width: 0;
      margin-inline: 8px;
      padding: 4px 10px;
      border-radius: 14px;
      background: var(--color-toolbar-background-subtle-emphasis);
      font-size: 13px;
      line-height: 20px;
      white-space: nowrap;
      overflow: hidden;
      text-overflow: ellipsis;
      user-select: text;
      cursor: text;
      -webkit-app-region: no-drag;
    }

    caption-button-container {
      flex: 0 0 auto;
      width: 144px;
      height: 34px;
      -webkit-app-region: no-drag;
    }

    main-contents-view-no-border {
      flex: 1 1 auto;
      min-height: 0;
    }
  `;

  override connectedCallback(): void {
    super.connectedCallback();
    tabsController.addTabsObserver(this.tabsObserver_);
    const proxy = BrowserApiProxyImpl.getInstance();
    this.fixedTitleListenerId_ =
        proxy.getNativeViewCallbackRouter().singleWebTitleChanged.addListener(
            (title: string) => {
              this.fixedTitle_ = title;
            });
    proxy.getSingleWebTitle().then(({title}) => {
      this.fixedTitle_ = title;
    });
  }

  override disconnectedCallback(): void {
    if (this.fixedTitleListenerId_ !== null) {
      BrowserApiProxyImpl.getInstance()
          .getNativeViewCallbackRouter()
          .removeListener(this.fixedTitleListenerId_);
      this.fixedTitleListenerId_ = null;
    }
    tabsController.removeTabsObserver(this.tabsObserver_);
    this.trackTab_(null);
    super.disconnectedCallback();
  }

  private trackTab_(tabId: TabId|null) {
    if (tabId === this.trackedTabId_) {
      return;
    }
    if (this.trackedTabId_ !== null) {
      tabsController.removeTabObserver(this.trackedTabId_, this.tabObserver_);
    }
    this.trackedTabId_ = tabId;
    if (tabId === null) {
      this.url_ = '';
      this.pageTitle_ = '';
      return;
    }
    tabsController.addTabObserver(tabId, this.tabObserver_);
    const tab = tabsController.getTab(tabId);
    if (tab) {
      this.url_ = shownAddress(tab.url);
      this.pageTitle_ = tab.title;
    }
  }

  protected override render() {
    const title = this.fixedTitle_ || this.pageTitle_;
    return html`
      <div id="titlebar">
        <img id="logo" alt="" src="nexus_welcome_logo.png">
        <div id="title" title="${title}">${title}</div>
        <caption-button-container align-x="right"></caption-button-container>
      </div>
      <div id="toolbar">
        <single-web-navigation-view></single-web-navigation-view>
        <div id="address" role="textbox" aria-readonly="true"
            aria-label="${loadTimeData.getString('singleWebAddressLabel')}"
            title="${this.url_}">${this.url_}</div>
      </div>
      <main-contents-view-no-border></main-contents-view-no-border>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'single-web-page': SingleWebPage;
  }
}

customElements.define(SingleWebPage.is, SingleWebPage);
