import {
  CrLitElement,
  css,
  html,
  type CSSResultGroup,
} from 'chrome://resources/lit/v3_0/lit.rollup.js';

/**
 * Shared layout wrapper for all Nexus subpages.
 *
 * Provides consistent horizontal padding, centered max-width content area,
 * and handles its own scrolling so the parent nexus-page never scrolls.
 *
 * Usage:
 *   <nexus-subpage-layout>
 *     <div>…subpage content…</div>
 *   </nexus-subpage-layout>
 */
export class NexusSubpageLayout extends CrLitElement {
  static get is() {
    return 'nexus-subpage-layout';
  }

  static override styles: CSSResultGroup = css`
    :host {
      display: flex;
      flex: 1 1 auto;
      flex-direction: column;
      min-height: 0;
      overflow-y: auto;
      overflow-x: hidden;
      width: 100%;
      -webkit-app-region: no-drag;
    }

    .layout-inner {
      box-sizing: border-box;
      margin: 0 auto;
      max-width: var(--cr-centered-card-max-width, 680px);
      min-width: 0;
      padding: 0;
      width: calc(100% * var(--cr-centered-card-width-percentage, 0.96));
    }

    @media (max-width: 980px) {
      .layout-inner {
        padding: 0 3px;
        width: 100%;
      }
    }
  `;

  protected override render() {
    return html`
      <div class="layout-inner">
        <slot></slot>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'nexus-subpage-layout': NexusSubpageLayout;
  }
}

customElements.define(NexusSubpageLayout.is, NexusSubpageLayout);
