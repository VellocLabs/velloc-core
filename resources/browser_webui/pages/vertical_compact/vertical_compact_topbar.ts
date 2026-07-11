import {CrLitElement, html, css, type CSSResultGroup} from 'chrome://resources/lit/v3_0/lit.rollup.js';
import '../../components/native/native_view_aliases.js';

export class VerticalCompactTopbar extends CrLitElement {
  static override styles: CSSResultGroup = css`
    :host {
      display: flex;
      align-items: center;
      width: 100%;
      overflow: hidden;
    }

    .cluster {
      flex: 1 1 auto;
      display: flex;
      align-items: center;
      min-height: 34px;
      overflow: hidden;
      width: 100%;
      min-width: 0;
      padding: 5px;
    }

    .cluster > * {
      min-width: 0;
      flex: 0 0 auto;
    }

    /* flex behavior for inner elements */
    .cluster > .location {
      flex: 1 1 0;
      min-width: 100px;
      width: auto;
    }

    .right-controls {
      display: flex;
      align-items: flex-start;
      flex: 0 0 auto;
      align-self: flex-start;
      gap: 0;
      margin-inline-start: 100px;
    }
  `;

  override render() {
    return html`
      <div class="cluster">
        <navigation-bar-view></navigation-bar-view>
        <location-extension-bar-views class="location"></location-extension-bar-views>
        <divider-view-dark align-x="right"></divider-view-dark>
        <pinned-downloads-button align-x="right"></pinned-downloads-button>
        <app-menu-button align-x="right"></app-menu-button>
      </div>
      <div class="right-controls">
        <glic-nexus-button align-x="right"></glic-nexus-button>
        <caption-button-container class="caption" align-x="right"></caption-button-container>
      </div>
    `;
  }
}

customElements.define('vertical-compact-topbar', VerticalCompactTopbar);
