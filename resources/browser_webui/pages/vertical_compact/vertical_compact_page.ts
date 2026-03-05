import {CrLitElement, html, css} from 'chrome://resources/lit/v3_0/lit.rollup.js';
import 'chrome://resources/cr_elements/cr_toggle/cr_toggle.js';
import {TabsMode} from '../../components/tabs_panel/types.js';
import type {VerticalTabsPanel} from '../../components/tabs_panel/vertical_tabs_panel.js';
import {installHoverSwap, type HoverSwapHandle} from '../../utils/hover_swap_util.js';
import {uiStore} from '../../ui_store.js';
import '../../components/native/native_view_aliases.js';
import '../../components/tabs_panel/vertical_tabs_panel.js';
import './vertical_compact_topbar.js';

export class VerticalCompactPage extends CrLitElement {
  private sidebarhoverSwapHandle_: HoverSwapHandle | null = null;
  private bottomBarHoverSwapHandle_: HoverSwapHandle | null = null;
  private sidebarHoverLocked_ = false;
  // private hoverObserver_: MutationObserver | null = null;
  private readonly sidebarHoverAttr_ = 'sidebar-hovered';
  private readonly bottomBarHoverAttr_ = 'bottombar-hovered';
  private readonly hoverLeaveDelayMs_ = 300;

  constructor() {
    super();
    this.sidebarHoverLocked_ = uiStore.leftPaneLocked;
  }

  static override styles = css`
    :host {
      display: flex;
      flex-direction: column;
      flex: 1 1 auto;
      min-height: 0;
      width: 100%;
      height: 100%;
      align-items: stretch;
      box-sizing: border-box;
      background: var(--app-bg);
      color: var(--text-primary);
      -webkit-app-region: drag;
      --bottom-bar-height: 40px;

      --vtp-icon-mode-width: 40px; /* width when in icon-only mode,34 + 2*3 margin */
      --vtp-icon-hover-mode-width: 200px;
      --vtp-list-radius: var(--app-radius);
      --vtp-separator: var(--google-grey-500);
    }



    #topContainer {
      display: flex;
      flex-direction: row;
      padding: 2px 5px;
      width: 100%;
      border-bottom: 1px solid var(--app-bg);
      align-items: center;
    }

    #topContainer pinned-tab-search-button {
      flex: 0 0 40px;
      width: 40px;
      height: 100%;
      display: flex;
      align-items: center;
      justify-content: center;
      box-sizing: border-box;
    }

    #topContainer vertical-compact-topbar {
      flex: 1 1 auto;
      min-width: 0;
      -webkit-app-region: drag;
    }

    #mainContents {
      display: flex;
      flex-direction: row;
      flex: 1 1 auto;
      min-height: 0;
      width: 100%;
      height: 100%;
      align-items: stretch;
    }

    #compact-sidebar {
      flex: 0 0 auto;
      margin: 0 5px;
      box-sizing: border-box;
      display: flex;
      flex-direction: column;
      height: 100%;
      min-height: 0;
      align-items: stretch;
      width: max-content;
      inline-size: max-content;
    }

    #sidebar-hover-area {
      display: flex;
      flex-direction: column;
      height: 100%;
      width: var(--vtp-icon-mode-width);
      inline-size: var(--vtp-icon-mode-width);
      transition: width 120ms ease-in-out;
    }

    :host([sidebar-hovered]) #sidebar-hover-area {
      width: var(--vtp-icon-hover-mode-width);
      inline-size: var(--vtp-icon-hover-mode-width);
    }

    vertical-tabs-panel {
      width: 100%;
      flex: 0 0 auto;
      height: auto !important;
      block-size: auto !important;
      min-height: 0;
      max-height: none;
      -webkit-app-region: no-drag;
    }

    #sidebar-header {
      padding: 2px 3px;
      display: flex;
      align-items: center;
      gap: 4px;
      justify-content: center;
      -webkit-app-region: no-drag;
    }

    #sidebar-hover-lock {
      margin-inline-start: auto;
    }

    :host(:not([sidebar-hovered="ended"]):not([sidebar-hovered="locked"])) #sidebar-hover-lock {
      display: none;
    }

    #mainContents #main {
      flex: 1 1 auto;
      -webkit-app-region: drag;
      height: 100%;
      min-height: 0;
      display: flex;
      flex-direction: column;
    }

    main-contents-view {
      flex: 1 1 auto;
      width: 100%;
    }

    #bottomBarHit {
      flex: 0 0 auto;
      margin-top: auto;
      height: var(--bottom-bar-height);
      overflow: hidden;
      width: 100%;
      display: flex;
      align-items: center;
      justify-content: flex-end;
      -webkit-app-region: no-drag;
      transition: height 120ms ease-in-out;
    }

    #bottomBarHit cr-icon {
      Display: block;
    }

    #bottomContainer {
      height: 0px;
      width: 100%;
      align-items: center;
      display: flex;
      transition: height 120ms ease-in-out;
    }

    pinned-toolbar-actions-center {
      width: 100%;
      height: 100%;
      -webkit-app-region: drag;
      display: none;
    }

    :host([bottombar-hovered]) #bottomBarHit cr-icon {
      display: none;
    }

    :host([bottombar-hovered]) #bottomContainer {
      height: var(--bottom-bar-height);
    }

    :host([bottombar-hovered=ended]) #bottomBarHit {
      height: 0px;
    }

    :host([bottombar-hovered=ended]) pinned-toolbar-actions-center {
      display: block;
    }
  `;

  protected override render() {
    return html`
      <div id="topContainer">
        <pinned-tab-search-button></pinned-tab-search-button>
        <vertical-compact-topbar></vertical-compact-topbar>
      </div>
      <div id="mainContents">
        <div id="compact-sidebar">
          <div id="sidebar-hover-area">
            <div id="sidebar-header">
              <split-tabs-button></split-tabs-button>
              <cr-toggle id="sidebar-hover-lock"
                  ?checked=${this.sidebarHoverLocked_}
                  @checked-changed=${this.onSidebarHoverLockChanged_}
                  aria-label="Lock sidebar hover">
              </cr-toggle>
            </div>
            <vertical-tabs-panel id="tabsPanel" mode="tabs_icon"></vertical-tabs-panel>
          </div>
          <div id="bottomBarHit">
           <cr-icon
              icon="cr:chevron-right">
           </cr-icon>
          </div>
        </div>
        <div id="main">
          <main-contents-view></main-contents-view>
        </div>
      </div>
      <div id="bottomContainer">
        <pinned-toolbar-actions-center x-align="stretch"></pinned-toolbar-actions-center>
      </div>
    `;
  }

  override firstUpdated() {
    this.installHoverSwap_();
    if (this.sidebarHoverLocked_) {
      this.applySidebarRevealState_(true, 'locked');
    }
  }

  override disconnectedCallback(): void {
    this.destroyHoverSwap_();
    super.disconnectedCallback();
  }

  private installHoverSwap_() {
    this.installSidebarHoverSwap_();
    this.installBottomBarHoverSwap_();
  }

  private installSidebarHoverSwap_(hasEntered = false) {
    if (this.sidebarHoverLocked_) {
      return;
    }
    const sidebarHover = this.shadowRoot?.getElementById('sidebar-hover-area');
    const tabsPanel = this.shadowRoot?.getElementById('tabsPanel') as VerticalTabsPanel | null;
    if (!sidebarHover || !tabsPanel) {
      return;
    }

    this.sidebarhoverSwapHandle_?.destroy();
    this.sidebarhoverSwapHandle_ = installHoverSwap({
      enterEl: sidebarHover,
      hoverEnter: (revealed) => {
        this.applySidebarRevealState_(revealed);
      },
      transitioned: () => {
        this.setAttribute(this.sidebarHoverAttr_, 'ended');
        return true;
      },
      hasEntered: hasEntered,
      leaveDelayMs: this.hoverLeaveDelayMs_,
    });
  }

  private installBottomBarHoverSwap_() {
    const bottomBar = this.shadowRoot?.getElementById('bottomContainer');
    const bottomBarHit = this.shadowRoot?.getElementById('bottomBarHit');
    if (!bottomBar || !bottomBarHit) {
      return;
    }
    this.bottomBarHoverSwapHandle_?.destroy();
    this.bottomBarHoverSwapHandle_ = installHoverSwap({
      enterEl: bottomBarHit,
      targetEl: bottomBar,
      hoverEnter: (revealed) => {
        if (revealed) {
          this.setAttribute(this.bottomBarHoverAttr_, '');
        } else {
          this.removeAttribute(this.bottomBarHoverAttr_);
        }
      },
      transitioned: () => {
        this.setAttribute(this.bottomBarHoverAttr_, 'ended');
        return true;
      },
      leaveDelayMs: this.hoverLeaveDelayMs_,
    });
  }

  private applySidebarRevealState_(revealed: boolean, attrValue = '') {
    if (revealed) {
      this.setAttribute(this.sidebarHoverAttr_, attrValue);
    } else {
      this.removeAttribute(this.sidebarHoverAttr_);
    }

    const tabsPanel = this.shadowRoot?.getElementById('tabsPanel') as VerticalTabsPanel | null;
    if (tabsPanel) {
      tabsPanel.mode = revealed ? TabsMode.TABS_ICON_LIST : TabsMode.TABS_ICON;
    }
  }

  private onSidebarHoverLockChanged_(event: CustomEvent<{value: boolean}>) {
    const locked = event.detail.value;
    if (locked === this.sidebarHoverLocked_) {
      return;
    }
    this.sidebarHoverLocked_ = locked;
    uiStore.setLeftPaneLocked(locked);

    this.sidebarhoverSwapHandle_?.destroy();
    this.sidebarhoverSwapHandle_ = null;

    if (locked) {
      this.applySidebarRevealState_(true, 'locked');
      return;
    }

    this.applySidebarRevealState_(false);
    this.installSidebarHoverSwap_(false);
  }

  private destroyHoverSwap_() {
    this.sidebarhoverSwapHandle_?.destroy();
    this.sidebarhoverSwapHandle_ = null;
    this.bottomBarHoverSwapHandle_?.destroy();
    this.bottomBarHoverSwapHandle_ = null;
  }
}

customElements.define('vertical-compact-page', VerticalCompactPage);
