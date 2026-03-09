import {CrLitElement, html, css, type CSSResultGroup} from 'chrome://resources/lit/v3_0/lit.rollup.js';
import {uiStore} from '../../ui_store.js';
import {installHoverSwap, type HoverSwapHandle} from '../../utils/hover_swap_util.js';
import type {CrSplitterElement} from 'chrome://resources/cr_elements/cr_splitter/cr_splitter.js';

import 'chrome://resources/cr_elements/cr_splitter/cr_splitter.js';
import '../../components/native/native_view_aliases.js';
import './vertical_topbar.js';
import './vertical_sidebar.js';

// A layout with a left sidebar (address bar + tabs) and a right content pane.
// Inspired by Arc/Zen vertical layout concept.
export class VerticalPage extends CrLitElement {
  static override styles: CSSResultGroup = css`
    :host {
      display: grid;
      grid-template-columns: var(--vertical-bar-left-width, 310px) var(--app-edge, 16px) minmax(0, 1fr);
      grid-template-rows: 100%;
      box-sizing: border-box;
      /* Use outer margin for page edge gap; keep only inter-column gap here */
      padding-inline: 0;
      column-gap: 0;
      /* Fill available height in parent flex column without overflowing margins */
      flex: 1 1 auto;
      min-height: 0;
      /* Let width be auto so horizontal margins are respected inside viewport */
      overflow: hidden;
      background: var(--app-bg);
      color: var(--text-primary);
      /* Edge margins: top=0, right/bottom/left=8px */
      margin: 0;
      box-shadow: 0 8px 24px rgba(0,0,0,.12);
      -webkit-app-region: drag;

      --vtp-list-active-shadow: none;
      --vtp-list-radius: 17px;
      --vtp-separator: var(--app-bg);
    }

    .div-container {
      background: var(--app-toolbar-bg);
      border-radius: 6px;
      /* x-offset, y-offset, blur, spread, color */
      box-shadow: 0 8px 24px rgba(0,0,0,.12);
      /* optional: padding/content */
      padding: 5px;
    }

    #left {
      display: flex;
      flex-direction: column;
      border-inline-end: 1px solid var(--color-web-ui-tab-strip-tab-separator, rgba(0,0,0,.1));
      min-width: 240px;
      position: relative;
      margin: var(--app-edge) 0 var(--app-edge) var(--app-edge);
      -webkit-app-region: no-drag;
    }

    #splitter {
      display: flex;
      align-items: stretch;
      justify-content: center;
      margin: var(--app-edge) 0;
      padding-inline: calc((var(--app-edge, 16px) - var(--vertical-bar-handle-width, 4px)) / 2);
      cursor: col-resize;
      touch-action: none;
      -webkit-app-region: no-drag;
      user-select: none;
      outline: none;
      position: relative;
      z-index: 1;
    }

    #splitter::before {
      content: '';
      width: var(--vertical-bar-handle-width, 4px);
      border-radius: 999px;
      background: var(--app-splitter-bg);
      opacity: 0;
      transform: scaleX(1);
      transition: opacity 120ms ease-in-out, transform 120ms ease-in-out, background 120ms ease-in-out;
    }

    #splitter.splitter-active::before,
    #splitter:hover::before,
    #splitter:focus-visible::before {
      opacity: 1;
    }

    /* Left panel implemented by vertical-sidebar component */
    #right {
      display: flex;
      flex-direction: column;
      overflow: hidden;
      -webkit-app-region: drag;
      box-sizing: border-box;
      padding: 0 0 var(--app-edge) 0;
      min-width: 0;
      gap: 0;
    }
    #contents {
      display: flex;
      flex-direction: column;
      align-items: stretch;
      flex: 1 1 auto;   /* fill remaining space */
      overflow: hidden; /* or auto, if you want scrolling inside */
      padding: 0 var(--app-edge) 0 0;
      -webkit-app-region: drag;
    }
    #contents > main-contents-view {
      flex: 1 1 auto;
      width: 100%;
      -webkit-app-region: no-drag;
    }

    #singleTopBar {
      flex: 0 0 auto;
      height: var(--app-edge);
      width: 100%;
      overflow: hidden;
      display: flex;
      align-items: center;
      pointer-events: auto;
      -webkit-app-region: no-drag;
      transition:
          height 120ms ease-in-out,
          transform 120ms ease-in-out;
    }

    vertical-topbar {
      display: none;
    }

    :host([topbar-show=ended]) vertical-topbar {
      display: flex;
    }

    :host([topbar-show]) #singleTopBar {
      display: flex;
      height: 40px;
      pointer-events: auto;
    }
  `;

  private leftPane_: HTMLElement|null = null;
  private splitter_: CrSplitterElement|null = null;
  private readonly minLeftWidth_ = 261;
  private readonly maxLeftWidth_ = 640;
  private readonly rightMinWidth_ = 320;
  private readonly keyboardStepPx_ = 16;
  private desiredLeftWidth_ = 310;
  private readonly onWindowResizeBound_ = () => this.onWindowResize_();
  private hoverSwapHandle_: HoverSwapHandle|null = null;

  constructor() {
    super();
    this.applyInitialLeftWidth_();
  }

  override firstUpdated() {
    const root = this.shadowRoot;
    if (!root) return;
    const bar = root.getElementById('singleTopBar');
    if (!(bar instanceof HTMLElement)) {
      return;
    }

    this.hoverSwapHandle_?.destroy();
    this.hoverSwapHandle_ = installHoverSwap({
      enterEl: bar,
      hoverEnter: (revealed) => {
        if (revealed) {
          this.setAttribute('topbar-show', 'started');
          return;
        }
        this.removeAttribute('topbar-show');
      },
      transitioned: ( propertyName) => {
        if (propertyName && propertyName !== 'height') {
          return false;
        }
        this.setAttribute('topbar-show', 'ended');
        return true;
      },
    });

    this.initResizer_();
    window.addEventListener('resize', this.onWindowResizeBound_);
  }

  private initResizer_() {
    const root = this.shadowRoot;
    if (!root) {
      return;
    }
    const left = root.getElementById('left');
    const splitter = root.getElementById('splitter');
    if (!(left instanceof HTMLElement) || !(splitter instanceof HTMLElement)) {
      this.leftPane_ = null;
      this.splitter_ = null;
      return;
    }
    this.leftPane_ = left;
    this.splitter_ = splitter as CrSplitterElement;

    splitter.addEventListener('dragmove', () => this.onSplitterDragMove_());
    splitter.addEventListener('resize', () => this.onSplitterResize_());
    splitter.addEventListener('keydown', (event: KeyboardEvent) => this.onSplitterKeyDown_(event));

    this.applyActualLeftWidth_(this.desiredLeftWidth_);
    this.updateSplitterAria_();
  }

  private applyInitialLeftWidth_() {
    if (uiStore.leftPaneWidth !== null) {
      this.desiredLeftWidth_ = this.clampDesiredLeftWidth_(uiStore.leftPaneWidth);
    }
    this.style.setProperty('--vertical-bar-left-width', `${this.desiredLeftWidth_}px`);
  }

  private onSplitterDragMove_() {
    this.syncLeftWidthFromSplitter_(false);
  }

  private onSplitterResize_() {
    this.syncLeftWidthFromSplitter_(true);
  }

  private onSplitterKeyDown_(event: KeyboardEvent) {
    if (!this.leftPane_) {
      return;
    }
    if (event.key !== 'ArrowLeft' && event.key !== 'ArrowRight') {
      return;
    }
    event.preventDefault();
    const currentWidth = this.leftPane_.getBoundingClientRect().width;
    const direction = event.key === 'ArrowLeft' ? -1 : 1;
    const step = event.shiftKey ? this.keyboardStepPx_ * 2 : this.keyboardStepPx_;
    this.setDesiredLeftWidth_(currentWidth + direction * step);
  }

  private syncLeftWidthFromSplitter_(persist: boolean) {
    if (!this.leftPane_) {
      return;
    }
    const width = this.leftPane_.getBoundingClientRect().width;
    this.setDesiredLeftWidth_(width, persist);
    this.leftPane_.style.width = '';
  }

  private setDesiredLeftWidth_(width: number, persist = true) {
    this.desiredLeftWidth_ = this.clampDesiredLeftWidth_(width);
    this.applyActualLeftWidth_(this.desiredLeftWidth_, persist);
  }

  private applyActualLeftWidth_(desiredWidth: number, persist = true) {
    const actualWidth = this.clampLeftWidthToViewport_(desiredWidth);
    const roundedWidth = Math.round(actualWidth);
    this.style.setProperty('--vertical-bar-left-width', `${roundedWidth}px`);
    this.updateSplitterAria_(roundedWidth);
    if (persist) {
      this.maybePersistLeftWidth_(roundedWidth);
    }
  }

  private maybePersistLeftWidth_(width: number) {
    if (!Number.isFinite(width)) {
      return;
    }
    uiStore.setLeftPaneWidth(width);
  }

  private clampDesiredLeftWidth_(width: number): number {
    return Math.round(Math.min(Math.max(width, this.minLeftWidth_), this.maxLeftWidth_));
  }

  private clampLeftWidthToViewport_(width: number): number {
    const dynamicMax = this.getMaxLeftWidth_();
    const bounded = Math.min(Math.max(width, this.minLeftWidth_), dynamicMax);
    return Math.round(bounded);
  }

  private getMaxLeftWidth_(): number {
    const hostWidth = this.getBoundingClientRect().width;
    const dynamicMax = hostWidth > 0 ? hostWidth - this.rightMinWidth_ : this.maxLeftWidth_;
    const bounded = Math.max(this.minLeftWidth_, Math.min(dynamicMax, this.maxLeftWidth_));
    return bounded;
  }

  private updateSplitterAria_(width?: number) {
    if (!this.splitter_) {
      return;
    }
    const effectiveWidth = width ?? this.leftPane_?.getBoundingClientRect().width;
    if (effectiveWidth !== undefined) {
      this.splitter_.setAttribute('aria-valuenow', `${Math.round(effectiveWidth)}`);
    }
    this.splitter_.setAttribute('aria-valuemin', `${this.minLeftWidth_}`);
    this.splitter_.setAttribute('aria-valuemax', `${Math.round(this.getMaxLeftWidth_())}`);
  }

  private onWindowResize_() {
    this.applyActualLeftWidth_(this.desiredLeftWidth_);
  }

  override disconnectedCallback(): void {
    window.removeEventListener('resize', this.onWindowResizeBound_);
    this.hoverSwapHandle_?.destroy();
    this.hoverSwapHandle_ = null;
    super.disconnectedCallback();
  }

  protected override render() {
    return html`
      <div id="left" class="div-container">
        <vertical-sidebar mode="preview"></vertical-sidebar>
      </div>
      <cr-splitter id="splitter"
        role="separator"
        aria-orientation="vertical"
        tabindex="0"
        aria-label="Resize sidebar">
      </cr-splitter>
      <div id="right">
        <div id="singleTopBar">
          <vertical-topbar></vertical-topbar>
        </div>
        <div id="contents">
          <main-contents-view>
          </main-contents-view>
        </div>
      </div>
    `;
  }
}

customElements.define('vertical-page', VerticalPage);
