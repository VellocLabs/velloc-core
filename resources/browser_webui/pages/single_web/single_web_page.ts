import {
  CrLitElement,
  css,
  html,
  type CSSResultGroup,
} from 'chrome://resources/lit/v3_0/lit.rollup.js';
import {loadTimeData} from 'chrome://resources/js/load_time_data.js';

import type {Tab} from '../../browser_controller.mojom-webui.js';
import '../../components/native/native_view_aliases.js';
import type {TabId} from '../../components/tabs_panel/types.js';
import {
  tabsController,
  type TabObserver,
  type TabsObserver,
} from '../../controller/tabs_controller.js';

/**
 * Single-web window (kSingleWeb, F-29): one tab, a read-only address, the
 * native back / forward / reload buttons and the OS caption buttons. The
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
    };
  }

  protected accessor url_: string = '';

  private trackedTabId_: TabId|null = null;

  private readonly tabsObserver_: TabsObserver = {
    onTabsUpdated: (pinned, unpinned) => {
      this.trackTab_(tabsController.getActiveTabId() ?? unpinned[0] ??
                     pinned[0] ?? null);
    },
  };

  private readonly tabObserver_: TabObserver = {
    onTabDataUpdated: (tab: Tab) => {
      this.url_ = tab.url;
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
      background: var(--app-bg);
      color: var(--text-primary);
    }

    #topbar {
      display: flex;
      align-items: center;
      flex: 0 0 auto;
      height: 37px;
      min-width: 0;
      padding-inline-start: 4px;
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
      background: var(--color-omnibox-background, rgba(127, 127, 127, 0.12));
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
      height: 37px;
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
  }

  override disconnectedCallback(): void {
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
      return;
    }
    tabsController.addTabObserver(tabId, this.tabObserver_);
    const tab = tabsController.getTab(tabId);
    if (tab) {
      this.url_ = tab.url;
    }
  }

  protected override render() {
    return html`
      <div id="topbar">
        <single-web-navigation-view></single-web-navigation-view>
        <div id="address" role="textbox" aria-readonly="true"
            aria-label="${loadTimeData.getString('singleWebAddressLabel')}"
            title="${this.url_}">${this.url_}</div>
        <caption-button-container align-x="right"></caption-button-container>
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
