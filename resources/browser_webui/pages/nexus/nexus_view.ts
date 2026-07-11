import {
  CrLitElement,
  css,
  html,
  type CSSResultGroup,
} from 'chrome://resources/lit/v3_0/lit.rollup.js';
import type {CrSplitterElement} from 'chrome://resources/cr_elements/cr_splitter/cr_splitter.js';

import 'chrome://resources/cr_elements/cr_splitter/cr_splitter.js';
import {uiStore} from '../../ui_store.js';

export class NexusViewElement extends CrLitElement {
  static get is() {
    return 'nexus-view';
  }

  static override styles: CSSResultGroup = css`
    :host {
      display: flex;
      flex: 1 1 auto;
      height: 100%;
      min-height: 0;
      width: 100%;
      -webkit-app-region: no-drag;
      overflow: hidden;
    }

    main-contents-view {
      flex: 1 1 auto;
      min-height: 0;
      width: 100%;
      min-width: 0;
      height: 100%;
    }

    #splitter {
      display: flex;
      align-items: stretch;
      justify-content: center;
      flex: 0 0 var(--app-edge, 10px);
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
      width: var(--nexus-splitter-handle-width, 4px);
      border-radius: 999px;
      background: var(--app-splitter-bg);
      opacity: 0;
      transform: scaleX(1);
      transition: opacity 120ms ease-in-out, transform 120ms ease-in-out,
          background 120ms ease-in-out;
    }

    #splitter.splitter-active::before,
    #splitter:hover::before,
    #splitter:focus-visible::before {
      opacity: 1;
    }

    nexus-view-container {
      flex: 0 0 auto;
      min-height: 0;
      min-width: 0;
      width: var(--nexus-view-width, 480px);
      height: 100%;
    }
  `;

  private nexusPane_: HTMLElement|null = null;
  private splitter_: CrSplitterElement|null = null;
  private readonly minNexusWidth_ = 360;
  private readonly maxNexusWidth_ = 960;
  private readonly mainContentsMinWidth_ = 480;
  private readonly keyboardStepPx_ = 16;
  private desiredNexusWidth_ = 480;
  private readonly onWindowResizeBound_ = () => this.onWindowResize_();

  constructor() {
    super();
    this.applyInitialNexusWidth_();
  }

  override firstUpdated() {
    this.initResizer_();
    window.addEventListener('resize', this.onWindowResizeBound_);
  }

  override disconnectedCallback(): void {
    window.removeEventListener('resize', this.onWindowResizeBound_);
    super.disconnectedCallback();
  }

  private initResizer_() {
    const root = this.shadowRoot;
    if (!root) {
      return;
    }

    const splitter = root.getElementById('splitter');
    const nexusPane = root.getElementById('nexusPane');
    if (!(splitter instanceof HTMLElement) || !(nexusPane instanceof HTMLElement)) {
      this.splitter_ = null;
      this.nexusPane_ = null;
      return;
    }

    this.splitter_ = splitter as CrSplitterElement;
    this.nexusPane_ = nexusPane;
    this.splitter_.resizeNextElement = false;

    splitter.addEventListener('dragmove', () => this.onSplitterDragMove_());
    splitter.addEventListener('resize', () => this.onSplitterResize_());
    splitter.addEventListener(
        'keydown', (event: KeyboardEvent) => this.onSplitterKeyDown_(event));

    this.applyActualNexusWidth_(this.desiredNexusWidth_);
    this.updateSplitterAria_();
  }

  private applyInitialNexusWidth_() {
    if (uiStore.nexusViewWidth !== null) {
      this.desiredNexusWidth_ =
          this.clampDesiredNexusWidth_(uiStore.nexusViewWidth);
    }
    this.style.setProperty(
        '--nexus-view-width', `${this.desiredNexusWidth_}px`);
  }

  private onSplitterDragMove_() {
    this.syncNexusWidthFromSplitter_(false);
  }

  private onSplitterResize_() {
    this.syncNexusWidthFromSplitter_(true);
  }

  private onSplitterKeyDown_(event: KeyboardEvent) {
    if (!this.nexusPane_) {
      return;
    }

    if (event.key !== 'ArrowLeft' && event.key !== 'ArrowRight') {
      return;
    }

    event.preventDefault();
    const currentWidth = this.nexusPane_.getBoundingClientRect().width;
    const direction = event.key === 'ArrowLeft' ? -1 : 1;
    const step =
        event.shiftKey ? this.keyboardStepPx_ * 2 : this.keyboardStepPx_;
    this.setDesiredNexusWidth_(currentWidth + direction * step);
  }

  private syncNexusWidthFromSplitter_(persist: boolean) {
    if (!this.nexusPane_) {
      return;
    }

    const width = this.nexusPane_.getBoundingClientRect().width;
    this.setDesiredNexusWidth_(width, persist);
    this.nexusPane_.style.width = '';
  }

  private setDesiredNexusWidth_(width: number, persist = true) {
    this.desiredNexusWidth_ = this.clampDesiredNexusWidth_(width);
    this.applyActualNexusWidth_(this.desiredNexusWidth_, persist);
  }

  private applyActualNexusWidth_(desiredWidth: number, persist = true) {
    const actualWidth = this.clampNexusWidthToViewport_(desiredWidth);
    const roundedWidth = Math.round(actualWidth);
    this.style.setProperty('--nexus-view-width', `${roundedWidth}px`);
    this.updateSplitterAria_(roundedWidth);
    if (persist) {
      this.maybePersistNexusWidth_(roundedWidth);
    }
  }

  private maybePersistNexusWidth_(width: number) {
    if (!Number.isFinite(width)) {
      return;
    }
    uiStore.setNexusViewWidth(width);
  }

  private clampDesiredNexusWidth_(width: number): number {
    return Math.round(
        Math.min(Math.max(width, this.minNexusWidth_), this.maxNexusWidth_));
  }

  private clampNexusWidthToViewport_(width: number): number {
    const dynamicMax = this.getMaxNexusWidth_();
    const bounded = Math.min(
        Math.max(width, this.minNexusWidth_), dynamicMax);
    return Math.round(bounded);
  }

  private getMaxNexusWidth_(): number {
    const hostWidth = this.getBoundingClientRect().width;
    const splitterWidth = this.splitter_?.getBoundingClientRect().width ?? 0;
    const dynamicMax =
        hostWidth > 0 ? hostWidth - this.mainContentsMinWidth_ - splitterWidth :
                        this.maxNexusWidth_;
    return Math.max(
        this.minNexusWidth_, Math.min(dynamicMax, this.maxNexusWidth_));
  }

  private updateSplitterAria_(width?: number) {
    if (!this.splitter_) {
      return;
    }

    const effectiveWidth =
        width ?? this.nexusPane_?.getBoundingClientRect().width;
    if (effectiveWidth !== undefined) {
      this.splitter_.setAttribute(
          'aria-valuenow', `${Math.round(effectiveWidth)}`);
    }
    this.splitter_.setAttribute('aria-valuemin', `${this.minNexusWidth_}`);
    this.splitter_.setAttribute(
        'aria-valuemax', `${Math.round(this.getMaxNexusWidth_())}`);
  }

  private onWindowResize_() {
    this.applyActualNexusWidth_(this.desiredNexusWidth_);
  }

  protected override render() {
    return html`
      <nexus-view-container id="nexusPane"></nexus-view-container>
      <cr-splitter id="splitter"
          role="separator"
          aria-orientation="vertical"
          tabindex="0"
          aria-label="Resize Nexus panel">
      </cr-splitter>
      <main-contents-view align-x="right"></main-contents-view>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'nexus-view': NexusViewElement;
  }
}

customElements.define(NexusViewElement.is, NexusViewElement);
