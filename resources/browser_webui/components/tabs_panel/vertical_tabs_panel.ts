import '/strings.m.js';
import {CrLitElement, css, html, type PropertyValues} from 'chrome://resources/lit/v3_0/lit.rollup.js';
import {TabsMode, type TabId} from './types.js';
import {verticalScrollbarStyles} from './vertical_tab_list/custom_scrollbar.js';
import './vertical_tab_list/tab_item.js';
import type {TabItem} from './vertical_tab_list/tab_item.js';
import '//resources/cr_elements/cr_icon/cr_icon.js';
import '//resources/cr_elements/icons.html.js';
import {BrowserApiProxyImpl} from '../../browser_api_proxy.js';
import {BrowserCommandID} from '../../browser_command_ids.mojom-webui.js';
import {loadTimeData} from 'chrome://resources/js/load_time_data.js';
import {tabsController, type TabsObserver} from '../../controller/tabs_controller.js';

enum ShowPinned {
    NONE = 'NONE',
    LIST = 'LIST',
    GRID = 'GRID',
}

export class VerticalTabsPanel extends CrLitElement implements TabsObserver {
  static override get properties() {
    return {
      show_pinned: {type: String, reflect: true},
      mode: {type: String, reflect: true},
    };
  }

  pinnedTabs: TabId[] = [];
  unpinnedTabs: TabId[] = [];
  accessor mode: TabsMode = TabsMode.TABS_PREVIE_LIST;
  accessor show_pinned: ShowPinned = ShowPinned.GRID;

  static override styles = [verticalScrollbarStyles, css`
    :host {
      display: flex;
      flex-direction: column;
      block-size: 100%;
      inline-size: 100%;
      min-block-size: 0;
      min-inline-size: 0;
      overflow: hidden;
      --cr-url-list-item-padding: 8px 7px 8px 5px;
    }

    #container {
      display: flex;
      flex-direction: column;
      flex: 1 1 auto;
      min-inline-size: 0;
      min-block-size: 0;
      overflow: hidden;
      inline-size: 100%;
    }

    #list {
      display: flex;
      flex-direction: column;
      flex: 0 1 auto;
      inline-size: 100%;
      min-inline-size: 0;
      min-block-size: 0;
      overflow-y: auto;
      overflow-x: hidden;
      scroll-behavior: smooth;
    }

    #unpinned_list,
    #pinned_list {
      display: flex;
      flex-direction: column;
      flex: 0 0 auto;
      inline-size: 100%;
      min-inline-size: 0;
      overflow: visible;
    }

    tab-item {
      flex: 0 0 auto;
      align-self: stretch;
    }

    #list:focus {
      outline: none;
    }

    #separator_list {
      display: flex;
    }

    :host([show_pinned="NONE"]) #separator_list,
    :host([show_pinned="NONE"]) #pinned_list {
      display: none;
    }

    :host([show_pinned="GRID"]) #pinned_list tab-item {
      inline-size: 34px;
      block-size: 34px;
      margin: 4px;
    }

    :host([show_pinned="GRID"]) #pinned_list tab-item:hover {
      box-shadow: var(--cr-elevation-1);
    }

    :host([show_pinned="GRID"]) #pinned_list {
      display: grid;
      grid-template-columns: repeat(auto-fit, 42px);
      gap: 10px;
      justify-content: start;
    }
    

    .separator {
      border: none;
      border-top: 1px solid var(--vtp-separator);
      margin: 2px 5px;
    }

    #addButtonContainer {
      display: flex;
      align-items: center;
      justify-content: flex-start;
      gap: 2px;
      padding-inline: 2px;
      cursor: pointer;
      user-select: none;
      background-color: transparent;
      border: 1px solid var(--app-bg);
      box-sizing: border-box;
      inline-size: auto;
      max-inline-size: 100%;

      margin: var(--vtp-list-margin, 3px);
      border-radius: var(--vtp-list-radius);
      height: var(--vtp-list-item-height, 34px);
    }

    :host([mode="tabs_icon"]) #addButtonContainer {
      justify-content: center;
      padding-inline: 0;
    }

    #addButtonContainer:hover {
      background-color: var(--vtp-list-hover-bg, var(--color-toolbar-background-subtle-emphasis-hovered));
      box-shadow: var(--vtp-list-hover-shadow, var(--cr-elevation-2));
    }

    #addButtonContainer cr-icon {
      --iron-icon-height: 24px;
      --iron-icon-width: 24px;
      flex: 0 0 auto;
    }

    #addButtonTitle {
      flex: 1 1 auto;
      min-inline-size: 0;
      white-space: nowrap;
      overflow: hidden;
      text-overflow: ellipsis;
      opacity: 0;
      visibility: hidden;
      display: none;
    }

    :host(:not([mode="tabs_icon"])) #addButtonContainer:hover #addButtonTitle {
      opacity: 1;
      visibility: visible;
      display: block;
    }

  `];


  override connectedCallback() {
    super.connectedCallback();
    tabsController.addTabsObserver(this);
  }

  override disconnectedCallback() {
    tabsController.removeTabsObserver(this);
    super.disconnectedCallback();
  }

  onTabsUpdated(pinnedTabs: TabId[], unpinnedTabs: TabId[]) {
    let changed = false;
    if (!this.sameTabLists(this.pinnedTabs, pinnedTabs)) {
      this.pinnedTabs = [...pinnedTabs];
      changed = true;
    }
    if (!this.sameTabLists(this.unpinnedTabs, unpinnedTabs)) {
      this.unpinnedTabs = [...unpinnedTabs];
      changed = true;
    }
    if (changed) {
      this.syncListsDom_();
    }
  }

  private sameTabLists(a: TabId[], b: TabId[]) {
    if (a.length !== b.length) {
      return false;
    }
    for (let i = 0; i < a.length; i++) {
      if (a[i] !== b[i]) {
        return false;
      }
    }
    return true;
  }

  private onAddButtonClick_() {
    const proxy = BrowserApiProxyImpl.getInstance();
    proxy.executeBrowserCommand(BrowserCommandID.IDC_BROWSER_NEW_TAB);
  }

  protected override render() {
    return html`
      <div id="container"
        @contextmenu=${this.onListContextMenu_}>
        <div class="list" id="list">
          <div id="separator_top" class="separator"></div>
          <div id="pinned_list"></div>
          <div id="separator_list" class="separator"></div>
          <div id="unpinned_list"></div>
        </div>
        <div id="footer">
          <div id="separator_add" class="separator"></div>
          <div id="addButtonContainer" @click=${this.onAddButtonClick_}>
            <cr-icon
              icon="cr:add">
            </cr-icon>
            <span id="addButtonTitle">${loadTimeData.getString('newTabAccName')}</span>
          </div>
        </div>
      </div>
    `;
  }

  override firstUpdated() {
    this.syncListsDom_();
  }

  override updated(changedProperties: PropertyValues<this>) {
    super.updated(changedProperties);
    if (changedProperties.has('mode')) {
      this.syncListsDom_();
    }
  }


  private syncListsDom_() {
    const pinned_list = this.shadowRoot?.getElementById('pinned_list');
    const unpinnedListEl = this.shadowRoot?.getElementById('unpinned_list');
    if ( this.pinnedTabs.length === 0 ) {
      this.show_pinned = ShowPinned.NONE;
    } else {
      this.show_pinned = this.mode === TabsMode.TABS_PREVIE_LIST ? ShowPinned.GRID : ShowPinned.LIST;
      this.syncListDom_(pinned_list, this.pinnedTabs, this.mode === TabsMode.TABS_PREVIE_LIST ? TabsMode.TABS_ICON : this.mode);
    }
    this.syncListDom_(unpinnedListEl, this.unpinnedTabs, this.mode);
  }

  private syncListDom_(listEl?: HTMLElement | null, tabIds?: TabId[], desiredMode?: TabsMode) {
    if (listEl === null || listEl === undefined || desiredMode === undefined) {
      return;
    }

    const desiredTabIds: TabId[] = [...(tabIds ?? [])];
    const existingById = new Map<number, TabItem>();
    Array.from(listEl.children).forEach(child => {
      if (child instanceof HTMLElement) {
        const item = child as TabItem;
        if (item.tabId !== undefined) {
          existingById.set(item.tabId, item);
        }
      }
    });

    for (let index = 0; index < desiredTabIds.length; index++) {
      const tabId = desiredTabIds[index]!;
      const current = listEl.children[index] as TabItem | undefined;

      if (current?.tabId === tabId) {
        current.mode = desiredMode;
        existingById.delete(tabId);
        continue;
      }

      let item = existingById.get(tabId);
      if (!item) {
        item = document.createElement('tab-item') as TabItem;
        item.tabId = tabId;
      } else {
        existingById.delete(tabId);
      }
      item.mode = desiredMode;
      listEl.insertBefore(item, listEl.children[index] ?? null);
    }

    for (const stale of existingById.values()) {
      stale.remove();
    }
  }

  private onListContextMenu_(event: MouseEvent) {
    event.preventDefault();
    event.stopPropagation();
    const proxy = BrowserApiProxyImpl.getInstance();
    proxy.showBackgroundContextMenu(
        Math.round(event.clientX), Math.round(event.clientY));
  }
}

customElements.define('vertical-tabs-panel', VerticalTabsPanel);
