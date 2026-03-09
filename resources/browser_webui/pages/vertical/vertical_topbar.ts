import {CrLitElement, html, css, type CSSResultGroup} from 'chrome://resources/lit/v3_0/lit.rollup.js';
import '../../components/native/native_view_aliases.js';
// A dedicated top bar for the Vertical layout. This is not reused from
// other modes to allow different controls/content.
export class VerticalTopbar extends CrLitElement {
  static override styles: CSSResultGroup = css`
    :host {
      display: flex;
      align-items: center;
      inline-size: 100%;
      background: var(--app-bg);
      color: var(--cr-toolbar-color);
      -webkit-app-region: drag;
    }
    .left { flex: 1 1 auto;
            -webkit-app-region: drag; }
  `;

  protected override render() {
    return html`
      <pinned-toolbar-actions class="left" x-align="stretch"></pinned-toolbar-actions>
      <caption-button-container></caption-button-container>
    `;
  }
}

customElements.define('vertical-topbar', VerticalTopbar);
