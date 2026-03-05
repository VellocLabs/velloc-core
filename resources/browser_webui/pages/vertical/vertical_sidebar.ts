import {CrLitElement, html, css} from 'chrome://resources/lit/v3_0/lit.rollup.js';
import '../../components/native/native_view_aliases.js';
import '../../components/tabs_panel/vertical_tabs_panel.js';

// Sidebar toolbar for Vertical mode.
// Contains navigation, address (LocationBarView), and a tabs area.
export class VerticalSidebar extends CrLitElement {
  static override styles = css`
    :host {
      display: flex;
      flex-direction: column;
      inline-size: 100%;
      block-size: 100%;
      background: var(--app-toolbar-bg);
    }
    #topBar {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 8px;
      padding: 0px;
      /*border-block-end: 1px solid var(--color-web-ui-tab-strip-tab-separator, rgba(0,0,0,.08));*/
      background: transparent;
      /* Ensure visible height and prevent collapsing */
      block-size: 36px;
      flex: 0 0 36px;
      position: relative;
      z-index: 1;
    }
    /* make the native view fill the group's width */
    #spacer {
      display: block;          /* custom elements are inline by default */
      inline-size: 100%;
      flex: 1 1 auto;
      min-inline-size: 0;
    }
    #addrRow {
      display: flex;
      align-items: center;
      gap: 4px;
      padding: 4px 0px;
      background: transparent;
    }
    .group { display: flex; align-items: center; gap: 5px; }
    .location {
      flex: 1 1 auto;
      block-size: 36px;
      min-inline-size: 0;
    }
    #tabsPane {
      flex: 1 1 auto;
      display: flex;
      flex-direction: column;
      overflow: hidden;
      background: transparent;
      padding: 4px 0px;
      gap: 0;
    }
    #bottomBar {
      display: flex;
      align-items: center;
      gap: 8px;
      padding: 4px 0px 0px 0px;
      border-block-start: 1px solid var(--app-bg);
      background: transparent;
    }
    #bottomBar .extensions {
      flex: 1 1 auto;
      min-inline-size: 0;
    }
    #bottomBar .extensions sidebar-bottom-bar-views {
      display: block;
      inline-size: 100%;
      flex: 1 1 auto;
    }
  `;
  protected override render() {
    return html`
      <!-- Sidebar Top Bar: left app/home, right back/forward/reload -->
      <div id="topBar" data-anchor-layout-host>
        <div class="group">
          <pinned-tab-search-button></pinned-tab-search-button>
        </div>
        <div id="spacer"></div>
        <div class="group">
          <navigation-bar-view></navigation-bar-view>
          <app-menu-button></app-menu-button>
        </div>
      </div>
      <div id="addrRow" data-anchor-layout-host>
        <location-bar-auto-size-view class="location"></location-bar-auto-size-view>
      </div>
      <div id="tabsPane">
        <vertical-tabs-panel mode="tabs_list"></vertical-tabs-panel>
      </div>
      <div id="bottomBar" data-anchor-layout-host>
        <div class="group left">
          <pinned-downloads-button align-y="bottom"></pinned-downloads-button>
          <divider-view  align-y="bottom"></divider-view>
        </div>
        <div class="group extensions">
          <sidebar-bottom-bar-views align-y="bottom"></sidebar-bottom-bar-views>
        </div>
      </div>
    `;
  }
}

customElements.define('vertical-sidebar', VerticalSidebar);
