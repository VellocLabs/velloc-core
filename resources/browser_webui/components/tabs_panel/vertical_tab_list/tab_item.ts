import '/strings.m.js';
import '//resources/cr_elements/cr_button/cr_button.js';
import '//resources/cr_elements/cr_collapse/cr_collapse.js';
import '//resources/cr_elements/cr_expand_button/cr_expand_button.js';
import '//resources/cr_elements/cr_icon/cr_icon.js';
import '//resources/cr_elements/cr_icon_button/cr_icon_button.js';
import '//resources/cr_elements/icons.html.js';
import '//resources/cr_elements/cr_checkbox/cr_checkbox.js';
import '//resources/cr_elements/cr_input/cr_input.js';
import '//resources/cr_elements/cr_url_list_item/cr_url_list_item.js';
import './cr_url_list_item_ex.js';

import {CrRippleMixin} from '//resources/cr_elements/cr_ripple/cr_ripple_mixin.js';
import {getHtml, getCSS} from './tab_item.html.js';
import {CrLitElement} from '//resources/lit/v3_0/lit.rollup.js';
import {getFavicon} from 'chrome://resources/js/icon.js';
import {loadTimeData} from 'chrome://resources/js/load_time_data.js';
import {TabMode, TabsMode, type TabId} from "../types.js";
import {tabHoverPreviewController} from "../tab_hover_preview_controller.js";
import {TabExtras, tabsController, type TabObserver} from '../../../controller/tabs_controller.js';
import type {Tab} from "../../../browser_controller.mojom-webui.js";
import {TabNetworkState} from "../../../tabs.mojom-webui.js";
import {BrowserApiProxyImpl} from "../../../browser_api_proxy.js";

import '../../tab_indicators/alert_indicators.js';
import type {AlertIndicatorsElement} from '../../tab_indicators/alert_indicators.js';


// const NAVIGATE_EVENT = "vertical-tab-row-arrow";
// const MOUSE_ENTER_EVENT = "vertical-tab-row-mouseenter";
// const MOUSE_LEAVE_EVENT = "vertical-tab-row-mouseleave";

// const DEFAULT_FAVICON_URL = "chrome://theme/IDR_DEFAULT_FAVICON";
const DELAY_TO_HIDE_PREVIEW_MS = 2000;

const CrTabItemElementBase = CrRippleMixin(CrLitElement);
export class TabItem extends CrTabItemElementBase implements TabObserver {
      static override get properties() {
        return {  
          item_mode: {type: String, reflect: true},
          mode:     {type: String, reflect: false, noAccessor: true},
          title:    {type: String},
          domain:   {type: String},
          openedCount: {type: Number},
          tabId:    {type: Number},
          thumbnailUrl: {type: String}
        };
      }
  protected accessor item_mode: TabMode =  TabMode.LIST; // 用于样式/渲染
  private _mode: TabsMode = TabsMode.TABS_PREVIE_LIST; // 外部设置，决定显示模式

  get mode(): TabsMode {
    return this._mode;
  }

  set mode(value: TabsMode) {
    const oldMode = this._mode;
    if (oldMode === value) {
      return;
    }
    this._mode = value;
    this.updateItemMode();
  }
  
  accessor tabId!: TabId;                   // 外部设置，标识对应 Tab
  override accessor title: string = '';             
  protected accessor domain: string = '';
  protected accessor thumbnailUrl: string | null = null;
  protected accessor openedCount: number = 0;
  private tabExtras: TabExtras | null = null; // 附加数据
  private observerAttached = false;
  static override styles = getCSS();
  private getAccessibleTitle(tab: Tab): string {
      const tabTitle = tab.title;

      if (tab.crashed) {
        return loadTimeData.getStringF('tabCrashed', tabTitle);
      }

      if (tab.networkState === TabNetworkState.kError) {
        return loadTimeData.getStringF('tabNetworkError', tabTitle);
      }

      return tabTitle;
  }
  // TabObserver
  onTabDataUpdated(tab: Tab) {
    this.domain = tab.domain;

    let faviconEl_ = this.shadowRoot?.getElementById('favicon');

    if( tab.splitStart === true || tab.splitEnd === true ) {
      this.setAttribute('split', tab.splitStart ? "start" : "end");
    } else {
      this.removeAttribute('split');
    }

    this.setAttribute('aria-label', this.getAccessibleTitle(tab));
    this.toggleAttribute('hide-icon_', !tab.showIcon);
    this.toggleAttribute(
        'waiting_',
        !tab.shouldHideThrobber &&
            tab.networkState === TabNetworkState.kWaiting);
    this.toggleAttribute(
        'loading_',
        !tab.shouldHideThrobber &&
            tab.networkState === TabNetworkState.kLoading);
    this.toggleAttribute('pinned', tab.pinned);
    this.toggleAttribute('blocked_', tab.blocked);
    this.setAttribute('draggable', String(true));
    this.toggleAttribute('crashed_', tab.crashed);


    if (tab.title) {
      this.title = tab.title;
    } else if (
        !tab.shouldHideThrobber &&
        (tab.networkState === TabNetworkState.kWaiting ||
          tab.networkState === TabNetworkState.kLoading)) {
      this.title = loadTimeData.getString('loadingTab');
    } else {
      this.title =
          loadTimeData.getString('defaultTabTitle');
    }
    
    if (faviconEl_) {
      if (tab.networkState === TabNetworkState.kWaiting ||
          (tab.networkState === TabNetworkState.kLoading &&
           tab.isDefaultFavicon)) {
        faviconEl_.style.backgroundImage = 'none';
      } else if (tab.faviconUrl) {
        const faviconUrl = tab.faviconUrl.url;
        faviconEl_.style.backgroundImage = `url(${faviconUrl})`;
      } else {
        faviconEl_.style.backgroundImage = getFavicon('');
      }
    }
    const alertStates = tab.alertStates;
    const alertIndicatorsEl_ =
        this.shadowRoot?.getElementById('alertIndicators') as
        AlertIndicatorsElement | null;
    if (alertIndicatorsEl_ && alertStates.length > 0) {
      customElements.upgrade(alertIndicatorsEl_);
      alertIndicatorsEl_.updateAlertStates(alertStates)
          .then((alertIndicatorsCount: number) => {
            this.toggleAttribute(
                'has-alert-states_', alertIndicatorsCount > 0);
          });
    } else {
      this.toggleAttribute('has-alert-states_', false);
    }
  }

  onTabExtrasUpdated(tabExtras: TabExtras) {
    this.tabExtras = tabExtras;
    this.openedCount = tabExtras.openedCount;
    this.updateItemMode();
  }

  updateItemMode() {
    let nextTabMode = TabMode.LIST;
    if (this._mode === TabsMode.TABS_ICON) {
      nextTabMode = TabMode.ICON;
    } else if (this._mode === TabsMode.TABS_PREVIE_LIST) {
      if ( (this.tabExtras?.openedCount ?? 0) < 1 ) {
        nextTabMode = TabMode.PREVIEW ;
      }
    }

    if (nextTabMode !== this.item_mode) {
      if (this.item_mode === TabMode.PREVIEW) {
         setTimeout(() => {
           if (this.item_mode === TabMode.PREVIEW) {
             this.item_mode = nextTabMode;
             tabsController.removeTrackedPreviewId(this.tabId);
             this.toggleAttribute('show_preview', false);
           }
         }, DELAY_TO_HIDE_PREVIEW_MS);
      } else {
        this.item_mode = nextTabMode;
        if (nextTabMode === TabMode.PREVIEW) {
          tabsController.addTrackedPreviewId(this.tabId);
        } 
      }
    }
  }

  onTabActiveStateChanged(active: boolean) {
    this.toggleAttribute('active', active);
    this.setAttribute('aria-selected', active ? 'true' : 'false');
  }

  onThumbnailChanged(thumbnailUrl: string | null) {
    if (this.tabId === undefined) {
      return;
    }

    this.thumbnailUrl = this.item_mode === TabMode.PREVIEW ? thumbnailUrl : null;
    this.toggleAttribute('show_preview', this.thumbnailUrl !== null);
  }
  // --- Lifecycle ------------------------------------------------------------
  constructor() {
    super();
    this.ensureRippleOnPointerdown();
  }

  override connectedCallback() {
    super.connectedCallback();
    this.attachTabObserverIfNeeded();
  }

  override firstUpdated() {
    this.attachTabObserverIfNeeded();
  }

  override disconnectedCallback() {
    if (this.observerAttached) {
      tabsController.removeTabObserver(this.tabId, this);
      this.observerAttached = false;
    }
    super.disconnectedCallback();
  }

  private attachTabObserverIfNeeded() {
    if (this.observerAttached ) {
      return;
    }
    tabsController.addTabObserver(this.tabId, this);
    this.observerAttached = true;
  }


  /** ------------- Render ------------- */

  public override render() {
    return getHtml.bind(this)();
  }

  // private renderCloseButton_() {
  //   return html`
  //     <button id="close" @click=${this.onClose_} aria-label="Close tab">
  //       <span id="closeIcon"></span>
  //     </button>
  //   `;
  // }

  // private renderList_() {
  //   return html`
  //     <div id="content">
  //       ${this.renderIcon_()}
  //       <span id="titleText"></span>
  //       <!-- <tabstrip-alert-indicators id="alertIndicators"></tabstrip-alert-indicators> -->
  //       ${this.renderCloseButton_()}
  //     </div>
  //   `;
  // }

  /** ------------- i18n ------------- */

  // private i18nCloseLabel_(title: string) {
  //   return `Close ${title}`;
  // }

  /** ------------- Event handlers ------------- */

  protected onActivate_(event: Event) {
    event.stopPropagation();
    tabsController.activateTab(this.tabId);
  }

  protected onClose_(event: Event) {
    event.stopPropagation();
    tabsController.closeTab(this.tabId);
  }

  // private onKeydown_(event: KeyboardEvent) {
  //   switch (event.key) {
  //     case "Enter":
  //     case " ":
  //       event.preventDefault();
  //       this.onActivate_(event);
  //       break;
  //     case "Delete":
  //     case "Backspace":
  //       event.preventDefault();
  //       this.onClose_(event);
  //       break;
  //     case "ArrowUp":
  //       event.preventDefault();
  //       this.dispatchNavigate_(-1);
  //       break;
  //     case "ArrowDown":
  //       event.preventDefault();
  //       this.dispatchNavigate_(1);
  //       break;
  //     case "Home":
  //       event.preventDefault();
  //       this.dispatchNavigate_(Number.MIN_SAFE_INTEGER);
  //       break;
  //     case "End":
  //       event.preventDefault();
  //       this.dispatchNavigate_(Number.MAX_SAFE_INTEGER);
  //       break;
  //   }
  // }

  // private dispatchNavigate_(direction: number) {
  //   this.dispatchEvent(new CustomEvent(NAVIGATE_EVENT, {
  //     detail: {direction},
  //     bubbles: true,
  //     composed: true,
  //   }));
  // }

  protected onMouseOver_(event: MouseEvent) {
    event.stopImmediatePropagation();
    event.preventDefault();
  }
  protected onMouseEnter_(event: MouseEvent) {
    const tabElement = event.currentTarget as HTMLElement | null;
    if (!tabElement) return;
    event.stopImmediatePropagation();
    event.preventDefault();
    tabHoverPreviewController.handleMouseEnter(
      this.tabId,
      this.getAnchorElement_(),
      tabElement,
      event
    );
  }

  protected onMouseLeave_(event: MouseEvent) {
    if (event.currentTarget == null) return;
    tabHoverPreviewController.handleMouseLeave(this.tabId);
  }

  private getAnchorElement_(): HTMLElement | null {
    const parent = this.parentElement;
    if (parent instanceof HTMLElement) {
      return parent;
    }
    const root = this.getRootNode();
    if (root instanceof ShadowRoot && root.host instanceof HTMLElement) {
      return root.host;
    }
    return this;
  }

  // private shouldIgnoreMouseLeave_(event: MouseEvent): boolean {
  //   const target = event.currentTarget;
  //   if (!(target instanceof HTMLElement)) return false;
  //   const rect = target.getBoundingClientRect();
  //   const {clientX, clientY} = event;

  //   let ret = clientX >= rect.left && clientX <= rect.right &&
  //       clientY >= rect.top && clientY <= rect.bottom;
  //   if (!ret) {
  //     console.error("shouldIgnoreMouseLeave_", {event, clientX, clientY, rect, ret});
  //   }
  //   return ret;
  // }

  protected onContextMenu_(event: MouseEvent) {
    event.preventDefault();
    event.stopPropagation();
    
    const proxy = BrowserApiProxyImpl.getInstance();
    proxy.showTabContextMenu(
      this.tabId,
      Math.round(event.clientX),
      Math.round(event.clientY)
    );
  }
}

customElements.define("tab-item", TabItem);
