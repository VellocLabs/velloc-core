import {
  CrLitElement,
  css,
  html,
  nothing,
  type CSSResultGroup,
} from 'chrome://resources/lit/v3_0/lit.rollup.js';
import type {CrSplitterElement} from 'chrome://resources/cr_elements/cr_splitter/cr_splitter.js';
import {addWebUiListener, removeWebUiListener} from
    'chrome://resources/js/cr.js';
import type {WebUiListener} from 'chrome://resources/js/cr.js';

import 'chrome://resources/cr_elements/cr_splitter/cr_splitter.js';
import './nexus_welcome.js';
import {bootstrapNexusGuest} from './nexus_runtime_bootstrap.js';
import {getString} from './nexus_shared.js';
import {uiStore} from '../../ui_store.js';

// Wall-clock fallback in case the React guest never sends
// nexus.ui.dismissBootWelcome (e.g. boot stalls in an error state). After
// this timeout the welcome is hidden so the user is never stuck staring at
// it, surfacing whatever state React is in (incl. its error UI).
// <main-contents-view> remains independent — it stays hidden until the
// model explicitly calls nexus.ui.showMainContents.
const DISMISS_BOOT_WELCOME_FALLBACK_MS = 10_000;

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
      /* Make the host a containing block for the boot-time absolutely
       * positioned children (welcome overlay + tiny-corner container) so
       * they pin to nexus-view's box, not the viewport. */
      position: relative;
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

    webview#nexusPane {
      flex: 0 0 auto;
      min-height: 0;
      min-width: 0;
      width: var(--nexus-view-width, 480px);
      height: 100%;
    }

    /* Boot-time: keep the <webview> at full layout size but visually
     * hidden so the inner React app lays out at the real conversation
     * viewport and produces compositor frames the entire time the
     * <nexus-welcome> overlay is up. Reveal is a pure visibility flip —
     * no compositor wake-up. The <webview> tag is a GuestView spawning a
     * separate guest process; visibility:hidden propagates correctly
     * through the embedder so the guest stays mounted but invisible. */
    webview#nexusPane.tiny-corner {
      visibility: hidden;
    }

    /* When main-contents-view + cr-splitter are hidden, the nexus pane is
     * the only visible surface in nexus-view. Stretch it across the full
     * host width instead of leaving it pinned to 480 px with empty space
     * to the right. The class is applied/removed by render() based on the
     * mainContentsVisible_ flag, so the pane snaps back to its user-resized
     * width as soon as the model calls nexus.ui.showMainContents. */
    webview#nexusPane.full-width {
      flex: 1 1 auto;
      width: 100%;
    }

    /* The welcome overlay covers the entire <nexus-view> host width during
     * boot. Splitter and main-contents-view are display:none until the
     * showHostLayout signal arrives, so the overlay isn't fighting them
     * for space and can occupy 100% of the host. */
    nexus-welcome.welcome-overlay {
      position: absolute;
      inset: 0;
      width: 100%;
      height: 100%;
      z-index: 10;
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

  // Two independent visibility flags — split per the user's design:
  //
  //   bootWelcomeDismissed_   Controls the <nexus-welcome> overlay AND the
  //                           <webview#nexusPane>'s `visibility`.
  //                           Flipped on the 'nexusDismissBootWelcome' event
  //                           (fired when the React guest calls
  //                           nexus.ui.dismissBootWelcome) or the fallback
  //                           timer.
  //
  //   mainContentsVisible_    Controls the <cr-splitter> + <main-contents-view>
  //                           pair. Stays false until the React-side model
  //                           explicitly calls nexus.ui.showMainContents — the
  //                           boot path does NOT touch this flag.
  //
  // Both flags latch: once true, stay true for the lifetime of the page.
  private bootWelcomeDismissed_ = false;
  private mainContentsVisible_ = false;
  // One-direction transient overlay set when a browser-category tool
  // dispatch reveals the agent zone. Computed visibility is
  // `transientAgentZoneShow_ || mainContentsVisible_`. The overlay is
  // turned on by `showAgentZoneTransient` and never auto-cleared — the
  // zone stays visible until the user manually clicks the globe, which
  // routes through `toggleAgentZone` and clears the overlay as part of
  // saving the new preference.
  private transientAgentZoneShow_ = false;
  private dismissBootWelcomeListener_: WebUiListener|null = null;
  private showMainContentsListener_: WebUiListener|null = null;
  private dismissBootWelcomeFallbackTimer_: number|null = null;

  constructor() {
    super();
    this.applyInitialNexusWidth_();
    // Register listeners as early as possible — before the element is even
    // attached — so a fast C++ broadcast cannot race past us. The cr.js
    // listener registration is independent of DOM attachment.
    this.dismissBootWelcomeListener_ = addWebUiListener(
        'nexusDismissBootWelcome',
        () => this.onDismissBootWelcome_('signal'));
    this.showMainContentsListener_ = addWebUiListener(
        'nexusShowMainContents',
        () => this.onShowMainContents_());
  }

  override firstUpdated() {
    this.initResizer_();
    window.addEventListener('resize', this.onWindowResizeBound_);
    if (this.dismissBootWelcomeFallbackTimer_ === null &&
        !this.bootWelcomeDismissed_) {
      this.dismissBootWelcomeFallbackTimer_ = setTimeout(
          () => this.onDismissBootWelcome_('timeout'),
          DISMISS_BOOT_WELCOME_FALLBACK_MS);
    }
    // Hand the guest <webview> off to the bootstrap controller. It sets the
    // src, runs the nexus-init handshake, calls
    // NexusBridgeBootstrap.guestReady(), and forwards the bridge ports
    // returned by the browser process to the webview. After that, the
    // SharedWorker inside the guest talks directly to NexusBridgePortService
    // — no chrome://glic intermediate.
    const webview = this.shadowRoot?.getElementById('nexusPane');
    if (webview) {
      void bootstrapNexusGuest(webview).catch((error: unknown) => {
        console.error('[NexusView] bootstrapNexusGuest failed', error);
      });
    } else {
      console.error('[NexusView] missing #nexusPane <webview> element');
    }
  }

  override disconnectedCallback(): void {
    window.removeEventListener('resize', this.onWindowResizeBound_);
    if (this.dismissBootWelcomeListener_) {
      removeWebUiListener(this.dismissBootWelcomeListener_);
      this.dismissBootWelcomeListener_ = null;
    }
    if (this.showMainContentsListener_) {
      removeWebUiListener(this.showMainContentsListener_);
      this.showMainContentsListener_ = null;
    }
    this.clearDismissBootWelcomeFallbackTimer_();
    super.disconnectedCallback();
  }

  private onDismissBootWelcome_(_reason: 'signal'|'timeout') {
    if (this.bootWelcomeDismissed_) {
      return;
    }
    this.bootWelcomeDismissed_ = true;
    this.clearDismissBootWelcomeFallbackTimer_();
    this.requestUpdate();
  }

  private onShowMainContents_() {
    if (this.mainContentsVisible_) {
      return;
    }
    this.mainContentsVisible_ = true;
    this.requestUpdate();
  }

  // Public toggle invoked by <nexus-page> when the topbar's agent-zone
  // button fires `nexus-toggle-agent-zone`. The boot path uses
  // `onShowMainContents_` (which only sets the flag true on the model's
  // initial reveal); after that, the user can flip the flag via this
  // method. The result is the new saved visibility — flips the OPPOSITE
  // of whatever the user is currently seeing (which may include a
  // transient overlay), then drops the overlay so the saved state takes
  // effect immediately. Fires `nexus-agent-zone-changed` so the topbar
  // can mirror the saved value into its globe-icon styling.
  public toggleAgentZone(): void {
    const wasVisible = this.transientAgentZoneShow_ || this.mainContentsVisible_;
    this.mainContentsVisible_ = !wasVisible;
    this.transientAgentZoneShow_ = false;
    this.requestUpdate();
    this.dispatchEvent(new CustomEvent('nexus-agent-zone-changed', {
      bubbles: true,
      composed: true,
      detail: {visible: this.mainContentsVisible_},
    }));
  }

  // Public hook for `<nexus-page>` to transiently reveal the agent zone
  // without mutating the user's saved preference. One-direction: there
  // is no matching "hide" entry point, so once a browser tool reveals
  // the zone it stays visible until the user manually clicks the globe
  // (which goes through `toggleAgentZone` and clears the overlay as
  // part of saving the new preference). No-op when the zone is already
  // visible (saved or otherwise).
  public showAgentZoneTransient(): void {
    if (this.transientAgentZoneShow_) {
      return;
    }
    this.transientAgentZoneShow_ = true;
    this.requestUpdate();
  }

  private clearDismissBootWelcomeFallbackTimer_() {
    if (this.dismissBootWelcomeFallbackTimer_ !== null) {
      clearTimeout(this.dismissBootWelcomeFallbackTimer_);
      this.dismissBootWelcomeFallbackTimer_ = null;
    }
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
    // <webview#nexusPane> stays mounted at all times; we never hide it
    // with visibility:hidden or display:none. While the boot welcome is up,
    // we tuck it into a 1x1 sliver pinned to the host's bottom-left corner
    // (covered by the welcome overlay). This keeps the guest <webview>
    // believing it is fully visible, so its compositor never throttles or
    // pauses frame production. When the boot signal arrives, we drop the
    // `tiny-corner` class and the container snaps back to its normal flex
    // position with no compositor wake-up to wait for.
    //
    // <cr-splitter> and <main-contents-view> use display:none and stay
    // hidden until the React guest's model calls nexus.ui.showMainContents.
    // While they're hidden, the container also gets the `full-width` class
    // so it occupies the entire host instead of just 480 px.
    // Both classes can apply during boot so the container occupies its
    // final post-reveal width WHILE hidden. Without `full-width` during
    // the tiny-corner phase, the container stays at the default 480 px
    // and the inner React app lays out at that narrow width. When the
    // welcome dismisses and only `full-width` applies, the container
    // grows to 100 % and the React conversation panel reflows — content
    // visibly "moves from left to center" as messages get max-w'd and
    // mx-auto starts producing real margins. Applying both keeps the
    // width constant across the visibility flip; the layout shift
    // disappears.
    const agentZoneVisible =
        this.transientAgentZoneShow_ || this.mainContentsVisible_;
    const containerClasses: string[] = [];
    if (!this.bootWelcomeDismissed_) {
      containerClasses.push('tiny-corner');
    }
    if (!agentZoneVisible) {
      containerClasses.push('full-width');
    }
    const containerClass = containerClasses.join(' ');
    const mainContentsHiddenStyle =
        agentZoneVisible ? '' : 'display: none;';
    return html`
      <webview id="nexusPane" class=${containerClass}
               partition="persist:nexus" allowtransparency="on">
      </webview>
      <cr-splitter id="splitter"
          role="separator"
          aria-orientation="vertical"
          tabindex="0"
          aria-label="Resize ${getString('customBrowserProductName', 'Nexus')} panel"
          style=${mainContentsHiddenStyle}>
      </cr-splitter>
      <main-contents-view align-x="right" style=${mainContentsHiddenStyle}>
      </main-contents-view>
      ${this.bootWelcomeDismissed_ ? nothing : html`
        <nexus-welcome class="welcome-overlay"></nexus-welcome>
      `}
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'nexus-view': NexusViewElement;
  }
}

customElements.define(NexusViewElement.is, NexusViewElement);
