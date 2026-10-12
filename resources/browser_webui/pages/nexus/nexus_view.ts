import {
  CrLitElement,
  css,
  html,
  nothing,
  type CSSResultGroup,
} from 'chrome://resources/lit/v3_0/lit.rollup.js';
import {
  onBootProgress,
  onBuildInfo,
  onDismissBootWelcome,
  onMainContentsLayout,
  type BootProgressPayload,
  type BuildInfoPayload,
  type MainContentsLayoutPayload,
} from './nexus_shell_client.js';

import {sendWithPromise} from 'chrome://resources/js/cr.js';

import './nexus_welcome.js';
import './nexus_guest_recovery.js';
import {LOADING_STAGE_ORDER, type LoadingStage} from './nexus_welcome.js';
import {bootstrapNexusGuest} from './nexus_runtime_bootstrap.js';

// Fallback for a guest that never dismisses the boot welcome. It bounds the
// time since the last progress signal (each one re-arms it), not total boot
// time, and is sized to clear the guest's initial boot with margin.
const DISMISS_BOOT_WELCOME_FALLBACK_MS = 25_000;

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
      /* The <webview> is allowtransparency; until the guest paints, this is
       * what shows through. Same neutral as <nexus-page> and the guest's own
       * pre-stylesheet ground. */
      background: var(--color-toolbar);
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

    /* Same geometry as the welcome overlay, one layer above it: the two are
     * mutually exclusive in render(), but the guest <webview> underneath may
     * be showing a 404 body and must stay covered. */
    nexus-guest-recovery.recovery-overlay {
      position: absolute;
      inset: 0;
      width: 100%;
      height: 100%;
      z-index: 11;
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
  // Latest guest-reported webview_area geometry, null until the first event.
  // Applied imperatively: the guest streams one event per frame during drags.
  private mainContentsLayout_: MainContentsLayoutPayload|null = null;
  private mainContentsEl_: HTMLElement|null = null;
  // Set when the guest is known not to be coming up: either the `nexus-init`
  // handshake never got an ack (bootstrapNexusGuest's onFailure — the case
  // that matters, because a wrong-but-valid URL yields a 404 the <webview>
  // loads SUCCESSFULLY) or the load failed at the network layer (loadabort).
  // Non-null renders <nexus-guest-recovery>, which is the only in-product way
  // back from a bad `nexus.webapp_url` — the pref's other editor lives in the
  // guest's own Settings · About page, i.e. inside the app that just failed.
  private guestFailureUrl_: string|null = null;
  // Why the guest failed (onGuestFailure_'s `reason`), for the offline test.
  private guestFailureReason_ = '';
  // `online` / `offline` on window: while the recovery overlay is up it
  // follows the connection (see isGuestOffline_), and coming back online
  // retries the guest by itself — the offline overlay promises exactly that.
  private readonly onConnectionChange_ = () => this.onConnectionChanged_();
  private dismissBootWelcomeUnsubscribe_: (() => void)|null = null;
  private mainContentsLayoutUnsubscribe_: (() => void)|null = null;
  private buildInfoUnsubscribe_: (() => void)|null = null;
  private bootProgressUnsubscribe_: (() => void)|null = null;
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
    this.bootProgressUnsubscribe_ = onBootProgress(
        payload => this.onBootProgress_(payload));
    window.addEventListener('online', this.onConnectionChange_);
    window.addEventListener('offline', this.onConnectionChange_);
  }

  override firstUpdated() {
    this.mainContentsEl_ =
        this.shadowRoot?.querySelector('main-contents-view') ?? null;
    this.applyMainContentsLayout_();
    this.armDismissBootWelcomeFallback_();
    // Hand the guest <webview> off to the bootstrap controller. It sets the
    // src, runs the nexus-init handshake, calls
    // NexusBridgeBootstrap.guestReady(), and forwards the bridge ports
    // returned by the browser process to the webview. After that, the
    // SharedWorker inside the guest talks directly to NexusBridgePortService
    // — no chrome://glic intermediate.
    const webview = this.shadowRoot?.getElementById('nexusPane');
    if (webview) {
      // `loadabort` only covers network-layer failures (DNS, refused
      // connection). It is a latency optimisation, not the primary trigger:
      // an HTTP 404 — the interface-skew case, where the origin is reachable
      // but does not serve this binary's /v<N>/ — is a SUCCESSFUL load that
      // never fires it. The handshake timeout below is what catches that.
      webview.addEventListener('loadabort', (e: Event) => {
        const detail = e as Event & {isTopLevel?: boolean, reason?: string};
        // Subframe aborts are the guest's business, and ERR_ABORTED is what
        // our own re-navigation produces — neither means the guest is dead.
        if (detail.isTopLevel === false || detail.reason === 'ERR_ABORTED') {
          return;
        }
        this.onGuestFailure_(null, `loadabort ${detail.reason ?? ''}`.trim());
      });
      void bootstrapNexusGuest(
          webview,
          stage => this.onLoadingStage_(stage),
          detail => this.onGuestFailure_(detail.guestUrl, detail.message))
          .catch((error: unknown) => {
            console.error('[NexusView] bootstrapNexusGuest failed', error);
          });
    } else {
      console.error('[NexusView] missing #nexusPane <webview> element');
    }
  }

  // Reveal the recovery overlay. Latches on the FIRST failure: the handshake
  // and loadabort can both fire for one dead load, and the first has the more
  // useful URL. Also dismisses the boot welcome — otherwise the splash would
  // sit there until its 25 s fallback while the recovery UI waits underneath.
  private onGuestFailure_(guestUrl: string|null, reason: string) {
    if (this.guestFailureUrl_ !== null) {
      return;
    }
    console.error('[NexusView] guest failed to start', {guestUrl, reason});
    this.guestFailureUrl_ = guestUrl ?? '';
    this.guestFailureReason_ = reason;
    this.bootWelcomeDismissed_ = true;
    this.clearDismissBootWelcomeFallbackTimer_();
    this.requestUpdate();
  }

  // Clears the `nexus.webapp_url` override (empty = built-in default) and
  // restarts the client, which restores the open session. The restart goes by
  // `chrome.send`: that route never answers, and the guest bridge is
  // unreachable from this overlay.
  private async onRecoveryReset_() {
    try {
      await sendWithPromise('setWebappUrl', '');
    } catch (e) {
      // Fall through to the restart anyway: if the pref write failed the panel
      // comes back to this same overlay, which is a better outcome than a
      // dead button.
      console.error('[NexusView] setWebappUrl reset failed', e);
    }
    chrome.send('restartBrowser');
  }

  // A browser-side reload (NexusSettingsHandler::HandleReloadPanel): the
  // page's own `location.reload()` is aborted on this WebUI and leaves the
  // overlay standing, which made Try again a dead button.
  private onRecoveryRetry_() {
    chrome.send('reloadPanel');
  }

  // Offline means the runtime source is not the suspect: the overlay then says
  // so instead of offering to reset it. `navigator.onLine` is the browser's own
  // view; ERR_INTERNET_DISCONNECTED is what a load aborted for the same reason
  // reports, in case the event and the abort race. A first launch with no
  // network is the case this is for — once the guest has loaded online it is
  // served from the HTTP cache and boots offline without ever getting here.
  private isGuestOffline_(): boolean {
    return navigator.onLine === false ||
        this.guestFailureReason_ === 'loadabort ERR_INTERNET_DISCONNECTED';
  }

  private onConnectionChanged_() {
    if (this.guestFailureUrl_ === null) {
      return;
    }
    if (navigator.onLine) {
      this.onRecoveryRetry_();
      return;
    }
    this.requestUpdate();
  }

  // Host-handshake stage source (bootstrapNexusGuest's onStatus callback):
  // starting → connecting → linking → loading.
  private onLoadingStage_(stage: LoadingStage) {
    this.armDismissBootWelcomeFallback_();
    this.advanceLoadingStage_(stage);
  }

  // Guest stage source (nexus.ui.bootProgress broadcast): the post-handshake
  // milestones the host can't observe (rendering, finalizing) PLUS keepalives
  // the SharedWorker re-emits during its long runtime build. The stage is a
  // free-form string on the wire so a newer guest can add stages without
  // breaking us; an unknown value is ignored by advanceLoadingStage_.
  private onBootProgress_(payload: BootProgressPayload) {
    // Re-arm on EVERY progress signal, not only forward advances: the worker
    // re-broadcasts its current stage every few seconds during phases with no
    // stage boundary of their own (MCP connect, transcript hydration, engine
    // build), so a slow-but-progressing build keeps the splash up even when a
    // single phase runs longer than the fallback window.
    this.armDismissBootWelcomeFallback_();
    this.advanceLoadingStage_(payload.stage as LoadingStage);
  }

  // Single funnel for BOTH stage sources. Advances the footer's loading-status
  // strictly FORWARD (by LOADING_STAGE_ORDER): the host handshake callback and
  // the guest's real-milestone `bootProgress` broadcasts feed it over two
  // channels, so a late-arriving earlier stage — or an unknown stage from a
  // newer guest — must never move the footer backwards. There is deliberately
  // NO timed advance: every stage change reflects a real boot event.
  private advanceLoadingStage_(stage: LoadingStage) {
    const next = LOADING_STAGE_ORDER.indexOf(stage);
    const current = LOADING_STAGE_ORDER.indexOf(this.loadingStage_);
    if (next < 0 || next <= current) {
      return;
    }
    this.loadingStage_ = stage;
    this.requestUpdate();
    // NB: the dismiss-fallback re-arm happens in the two signal entry points
    // (onLoadingStage_ / onBootProgress_) so EVERY progress signal — including
    // the worker's same-stage keepalives — re-arms it, not just forward advances.
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
    if (this.bootProgressUnsubscribe_) {
      this.bootProgressUnsubscribe_();
      this.bootProgressUnsubscribe_ = null;
    }
    this.clearDismissBootWelcomeFallbackTimer_();
    window.removeEventListener('online', this.onConnectionChange_);
    window.removeEventListener('offline', this.onConnectionChange_);
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

  // (Re)arm the dismiss fallback from now, unless the welcome is already
  // dismissed. Called at boot and on every real loading-stage advance, so the
  // window bounds "time since the last sign of progress" rather than total boot
  // time — a slow-but-progressing boot keeps the splash up until the guest
  // signals ready. Clearing first makes each call a reset, not a stack of timers.
  private armDismissBootWelcomeFallback_() {
    if (this.bootWelcomeDismissed_) {
      return;
    }
    this.clearDismissBootWelcomeFallbackTimer_();
    this.dismissBootWelcomeFallbackTimer_ = setTimeout(
        () => this.onDismissBootWelcome_('timeout'),
        DISMISS_BOOT_WELCOME_FALLBACK_MS);
  }

  private clearDismissBootWelcomeFallbackTimer_() {
    if (this.dismissBootWelcomeFallbackTimer_ !== null) {
      clearTimeout(this.dismissBootWelcomeFallbackTimer_);
      this.dismissBootWelcomeFallbackTimer_ = null;
    }
  }

  protected override render() {
    // <webview#nexusPane> stays mounted and full-bleed; <main-contents-view> is
    // an overlay tracking the guest's reports. While the boot welcome is up the
    // webview keeps its layout size but is hidden (`tiny-corner`), so reveal is
    // a pure visibility flip.
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
      ${this.guestFailureUrl_ === null ? nothing : html`
        <nexus-guest-recovery class="recovery-overlay"
            .guestUrl=${this.guestFailureUrl_}
            .offline=${this.isGuestOffline_()}
            @nexus-guest-recovery-reset=${this.onRecoveryReset_}
            @nexus-guest-recovery-retry=${this.onRecoveryRetry_}>
        </nexus-guest-recovery>
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
