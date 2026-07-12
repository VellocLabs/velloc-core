import {
  CrLitElement,
  css,
  html,
  nothing,
  type CSSResultGroup,
} from 'chrome://resources/lit/v3_0/lit.rollup.js';
import {
  onBuildInfo,
  onDismissBootWelcome,
  onMainContentsLayout,
  type BuildInfoPayload,
  type MainContentsLayoutPayload,
} from './nexus_ui_event_client.js';

import './nexus_welcome.js';
import type {LoadingStage} from './nexus_welcome.js';
import {bootstrapNexusGuest} from './nexus_runtime_bootstrap.js';

// Wall-clock fallback in case the React guest never sends
// nexus.ui.dismissBootWelcome (e.g. boot stalls in an error state). After
// this timeout the welcome is hidden so the user is never stuck staring at
// it, surfacing whatever state React is in (incl. its error UI).
// <main-contents-view> is independent — it tracks the guest's
// `mainContentsLayout` events (see below).
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
      /* Make the host a containing block for the absolutely positioned
       * children: the boot-time welcome overlay AND the
       * <main-contents-view> overlay (whose inline left/top/width/height
       * are written in host coordinates by applyMainContentsLayout_). */
      position: relative;
      -webkit-app-region: no-drag;
      overflow: hidden;
    }

    /* The native main-contents-view holder. The guest owns the geometry:
     * each session's webview_area placeholder (MultiSessionShell) reports
     * its rect through the SharedWorker as nexus.ui.mainContentsLayout,
     * and applyMainContentsLayout_() mirrors it into this element's inline
     * styles. The NativeViewHolder machinery then measures the element
     * (ResizeObserver + style-attribute MutationObserver) and pushes the
     * bounds + visibility over Mojo — display:none reports visible:false.
     * Hidden until the first visible report; no flex participation. */
    main-contents-view {
      position: absolute;
      display: none;
    }

    webview#nexusPane {
      flex: 1 1 auto;
      min-height: 0;
      min-width: 0;
      width: 100%;
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

    /* The welcome overlay covers the entire <nexus-view> host width during
     * boot. The guest <webview> is always full-bleed underneath it. */
    nexus-welcome.welcome-overlay {
      position: absolute;
      inset: 0;
      width: 100%;
      height: 100%;
      z-index: 10;
    }
  `;

  // Latches once: controls the <nexus-welcome> overlay AND the
  // <webview#nexusPane>'s visibility. Flipped on the
  // 'nexusDismissBootWelcome' event (fired when the React guest calls
  // nexus.ui.dismissBootWelcome) or the fallback timer.
  private bootWelcomeDismissed_ = false;
  // Guest bundle build time (ISO string), pushed once by the guest early in
  // boot via nexus.ui.buildInfo. Empty until it arrives; passed down to the
  // <nexus-welcome> overlay whose footer renders it. Applied via
  // requestUpdate() (like bootWelcomeDismissed_) — a single low-frequency
  // event, not a per-frame stream.
  private buildTime_ = '';
  // Current boot stage, advanced by the host handshake (bootstrapNexusGuest)
  // and passed to <nexus-welcome> for the footer's live loading-status
  // segment. Applied via requestUpdate() like the other overlay inputs.
  private loadingStage_: LoadingStage = 'starting';
  // Latest guest-reported webview_area geometry. Null until the first
  // event. Applied imperatively (not via render()) — splitter drags in
  // the guest stream one event per frame and a full lit re-render per
  // frame would be wasteful next to four inline-style writes.
  //
  // The pre-refactor flags this replaces (`mainContentsVisible_` latch +
  // `transientAgentZoneShow_` + the toggleAgentZone/showAgentZoneTransient
  // entry points + the <cr-splitter> and its width persistence via
  // IntKey.kNexusViewWidth) are all gone: visibility is now purely
  // automatic per session (visible ⟺ the active session owns a tab in
  // this kNexus window) and the split lives INSIDE the guest
  // (MultiSessionShell's webview_area splitter).
  private mainContentsLayout_: MainContentsLayoutPayload|null = null;
  private mainContentsEl_: HTMLElement|null = null;
  private dismissBootWelcomeUnsubscribe_: (() => void)|null = null;
  private mainContentsLayoutUnsubscribe_: (() => void)|null = null;
  private buildInfoUnsubscribe_: (() => void)|null = null;
  private dismissBootWelcomeFallbackTimer_: number|null = null;

  constructor() {
    super();
    // Register listeners as early as possible — before the element is even
    // attached — so a fast broadcast can't race past us. The bridge
    // transport attaches lazily on the first subscribe and reuses the
    // same connection across both event types. (The SharedWorker also
    // replays the last mainContentsLayout to each newly attached WebUI
    // port, so even a missed live broadcast self-heals on attach.)
    this.dismissBootWelcomeUnsubscribe_ = onDismissBootWelcome(
        () => this.onDismissBootWelcome_('signal'));
    this.mainContentsLayoutUnsubscribe_ = onMainContentsLayout(
        payload => this.onMainContentsLayout_(payload));
    this.buildInfoUnsubscribe_ = onBuildInfo(
        payload => this.onBuildInfo_(payload));
  }

  override firstUpdated() {
    this.mainContentsEl_ =
        this.shadowRoot?.querySelector('main-contents-view') ?? null;
    this.applyMainContentsLayout_();
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
      void bootstrapNexusGuest(webview, stage => this.onLoadingStage_(stage))
          .catch((error: unknown) => {
            console.error('[NexusView] bootstrapNexusGuest failed', error);
          });
    } else {
      console.error('[NexusView] missing #nexusPane <webview> element');
    }
  }

  private onLoadingStage_(stage: LoadingStage) {
    if (stage === this.loadingStage_) {
      return;
    }
    this.loadingStage_ = stage;
    this.requestUpdate();
  }

  override disconnectedCallback(): void {
    if (this.dismissBootWelcomeUnsubscribe_) {
      this.dismissBootWelcomeUnsubscribe_();
      this.dismissBootWelcomeUnsubscribe_ = null;
    }
    if (this.mainContentsLayoutUnsubscribe_) {
      this.mainContentsLayoutUnsubscribe_();
      this.mainContentsLayoutUnsubscribe_ = null;
    }
    if (this.buildInfoUnsubscribe_) {
      this.buildInfoUnsubscribe_();
      this.buildInfoUnsubscribe_ = null;
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
    // A layout that arrived during boot (e.g. the SharedWorker's
    // replay-on-attach of a pre-reload session's geometry) was stored but
    // deliberately not applied — apply it now that the guest is visible.
    this.applyMainContentsLayout_();
  }

  private onMainContentsLayout_(payload: MainContentsLayoutPayload) {
    this.mainContentsLayout_ = payload;
    this.applyMainContentsLayout_();
  }

  private onBuildInfo_(payload: BuildInfoPayload) {
    if (payload.buildTime === this.buildTime_) {
      return;
    }
    this.buildTime_ = payload.buildTime;
    // Only the overlay renders it, so a re-render after dismissal is wasted;
    // still cheap and harmless (the overlay is gone from the tree by then).
    this.requestUpdate();
  }

  // Mirrors the guest-reported rect into <main-contents-view>'s inline
  // styles, converting guest-viewport coords (1:1 with this page's CSS px —
  // same assumption as ui.setWindowDragRegions) into host-relative offsets
  // for the absolute positioning. While the boot welcome is up the view
  // stays hidden regardless of reports: the SharedWorker survives panel
  // reloads and may replay a stale visible layout from the previous panel
  // life before the rebooting guest posts a fresh state.
  private applyMainContentsLayout_() {
    const el = this.mainContentsEl_;
    if (!el) {
      return;
    }
    const layout = this.mainContentsLayout_;
    const rect = layout?.rect;
    if (!this.bootWelcomeDismissed_ || !layout?.visible || !rect) {
      el.style.display = 'none';
      return;
    }
    const hostRect = this.getBoundingClientRect();
    const webview = this.shadowRoot?.getElementById('nexusPane');
    const baseRect = webview ? webview.getBoundingClientRect() : hostRect;
    el.style.display = 'block';
    el.style.left =
        `${Math.round(baseRect.left - hostRect.left + rect.left)}px`;
    el.style.top = `${Math.round(baseRect.top - hostRect.top + rect.top)}px`;
    el.style.width = `${Math.round(rect.width)}px`;
    el.style.height = `${Math.round(rect.height)}px`;
  }

  // Phase 4: the panel no longer hosts settings (moved into the React guest).
  // `<nexus-page>` calls this to forward the C++ `ui.openNexusSettings`
  // deep-link to the guest <webview>, whose React `MultiSessionShell` listens
  // for `nexus-settings-deep-link` and opens the matching section. Best-effort
  // — a no-op if the guest webview hasn't attached its contentWindow yet.
  public forwardSettingsDeepLink(section: string): void {
    const webview = this.shadowRoot?.getElementById('nexusPane') as
        (HTMLElement & {contentWindow?: Window | null}) | null;
    webview?.contentWindow?.postMessage(
        {type: 'nexus-settings-deep-link', section}, '*');
  }

  private clearDismissBootWelcomeFallbackTimer_() {
    if (this.dismissBootWelcomeFallbackTimer_ !== null) {
      clearTimeout(this.dismissBootWelcomeFallbackTimer_);
      this.dismissBootWelcomeFallbackTimer_ = null;
    }
  }

  protected override render() {
    // <webview#nexusPane> stays mounted at all times and is always
    // full-bleed — the chat/browser split now lives INSIDE the guest
    // (MultiSessionShell's per-session webview_area + splitter), with
    // <main-contents-view> as an absolutely positioned overlay tracking
    // the guest's reports (see applyMainContentsLayout_).
    //
    // While the boot welcome is up the webview keeps its full layout size
    // but is visually hidden via the `tiny-corner` class, so the inner
    // React app lays out at the real conversation viewport and produces
    // compositor frames the whole time; reveal is a pure visibility flip.
    return html`
      <webview id="nexusPane"
               class=${this.bootWelcomeDismissed_ ? '' : 'tiny-corner'}
               partition="persist:nexus" allowtransparency="on">
      </webview>
      <main-contents-view></main-contents-view>
      ${this.bootWelcomeDismissed_ ? nothing : html`
        <nexus-welcome class="welcome-overlay"
            .buildTime=${this.buildTime_}
            .loadingStage=${this.loadingStage_}></nexus-welcome>
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
