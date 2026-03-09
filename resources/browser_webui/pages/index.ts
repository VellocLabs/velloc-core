import {CrLitElement, html, css, type CSSResultGroup} from 'chrome://resources/lit/v3_0/lit.rollup.js';
import 'chrome://resources/cr_elements/cr_shared_vars.css.js';
import {ColorChangeUpdater} from 'chrome://resources/cr_components/color_change_listener/colors_css_updater.js';
import './vertical/vertical_page.js';
import './vertical_compact/vertical_compact_page.js';
import './horizontal_compact/horizontal_compact_page.js';
import {uiStore, type UiStoreObserver} from '../ui_store.js';
import {UIMode} from '../ui_view_controller.mojom-webui.js';
import { errorReporter } from '../error_reporter.js';


export class MainApp extends CrLitElement {
  static override styles: CSSResultGroup = css`
    :host {
      display: flex;
      height: 100%;
      width: 100%;
      overflow: hidden;
      background: var(--app-bg);
      color: var(--text-primary);
    }
  `;

  mode: UIMode = uiStore.mode;
  private readonly uiStoreObserver_: UiStoreObserver = {
    onUiModeChanged: (mode) => this.switchPage_(mode),
  };

  override firstUpdated() {
    ColorChangeUpdater.forDocument().start();
    uiStore.addObserver(this.uiStoreObserver_);
    errorReporter.start();
  }

  override disconnectedCallback(): void {
    uiStore.removeObserver(this.uiStoreObserver_);
    super.disconnectedCallback();
  }

  private switchPage_(mode: UIMode) {
    if (this.mode === mode) {
      return;
    }
    this.mode = mode;
    this.requestUpdate();
  }

  protected override render() {
    return html`
      ${this.mode === UIMode.kSidebar ? html`<vertical-page></vertical-page>` :
        this.mode === UIMode.kCompactSidebar ? html`<vertical-compact-page></vertical-compact-page>` :
        this.mode === UIMode.kCompactStandard ? html`<horizontal-compact-page></horizontal-compact-page>` :
        html`<div></div>`}
    `;
  }
}

customElements.define('main-app', MainApp);
