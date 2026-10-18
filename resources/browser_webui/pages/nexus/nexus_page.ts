import {
  CrLitElement,
  css,
  html,
  type CSSResultGroup,
} from 'chrome://resources/lit/v3_0/lit.rollup.js';

import '../../components/native/native_view_aliases.js';

import {isNexusFirstRun} from './nexus_shared.js';
import './nexus_view.js';
import type {NexusViewElement} from './nexus_view.js';

// Longest a first run keeps the window's caption buttons hidden.
export const CAPTION_BUTTONS_HIDDEN_MAX_MS = 90_000;

// Session-list custom element. Registered for global use so any surface
// inside this page (today: nothing renders it by default) receives
// `nexusSessionsSummary` updates from the SharedWorker.

/**
 * Thin panel host: the `<nexus-view>` webview host, plus the
 * `window.customBrowserOpenNexusSettings` / `?settings=` deep-link forwarded to
 * the guest.
 */
export class NexusPage extends CrLitElement {
  static override styles: CSSResultGroup = css`
    :host {
      display: flex;
      flex-direction: column;
      flex: 1 1 auto;
      min-height: 0;
      width: 100%;
      height: 100%;
      align-items: stretch;
      box-sizing: border-box;
      overflow: hidden;
      /* Toolbar colour, not the tab-strip tint: this host is the ground the
       * welcome overlay and the guest paint over, so the intermediate frames
       * stay one neutral until the final colours land. */
      background: var(--color-toolbar);
      color: var(--text-primary);
    }

    #pane {
      display: flex;
      flex-direction: column;
      flex: 1 1 auto;
      margin: 0;
      min-height: 0;
      overflow: hidden;
    }

    #pane > * {
      display: flex;
      flex: 1 1 auto;
      min-height: 0;
    }

    #pane > [hidden] {
      display: none !important;
    }

    /* OS caption controls (min/max/close). A native Views widget
     * (BrowserCaptionButtonContainer) that cannot live inside the guest
     * <webview>, so the panel host paints it over the window's top-right —
     * overlaying the webview's top-right corner. The React topbar
     * (nexus_web) reserves a matching --nexus-caption-inset gap so its "Open
     * browser window" button aligns just left of these. Was previously
     * declared inside the (now-deleted) Lit <nexus-topbar>; relocated here so
     * it is present for the webview host.
     * align-x=right makes the native batcher anchor it to the window's right
     * edge; fixed positioning pins the holder's measured box there. */
    caption-button-container {
      position: fixed;
      top: 0;
      right: 0;
      width: 144px;
      height: 37px;
      z-index: 30;
      -webkit-app-region: no-drag;
    }

    /* The holder reports display:none as not visible, which hides the native
     * buttons. Needed explicitly: the holder's :host{display:block} outranks
     * the UA [hidden] rule. */
    caption-button-container[hidden] {
      display: none;
    }
  `;

  // A first run's boot welcome has no caption buttons, so the user cannot
  // close the window mid-setup; they return when the welcome goes away,
  // including when the recovery overlay replaces it, and at the latest after
  // CAPTION_BUTTONS_HIDDEN_MAX_MS, since a stalled boot can keep the welcome up.
  private captionButtonsHidden_ = isNexusFirstRun();
  private captionButtonsCapTimer_: number|null = null;

  private onBootWelcomeClosed_() {
    this.clearCaptionButtonsCap_();
    if (!this.captionButtonsHidden_) {
      return;
    }
    this.captionButtonsHidden_ = false;
    this.requestUpdate();
  }

  private clearCaptionButtonsCap_() {
    if (this.captionButtonsCapTimer_ !== null) {
      clearTimeout(this.captionButtonsCapTimer_);
      this.captionButtonsCapTimer_ = null;
    }
  }

  private readonly openSettingsFromHost_ = (section?: string) => {
    this.forwardSettingsDeepLink_(this.normalizeSection_(section));
  };

  override connectedCallback(): void {
    super.connectedCallback();
    if (this.captionButtonsHidden_ && this.captionButtonsCapTimer_ === null) {
      this.captionButtonsCapTimer_ = setTimeout(
          () => this.onBootWelcomeClosed_(), CAPTION_BUTTONS_HIDDEN_MAX_MS);
    }
    window.customBrowserOpenNexusSettings = this.openSettingsFromHost_;
    // Deep-link: rejection messages from native tools include a chrome://nexus
    // URL with `?settings=<section>`. Forward it to the guest's React settings.
    const params = new URLSearchParams(window.location.search);
    const requested = params.get('settings');
    if (requested) {
      this.forwardSettingsDeepLink_(this.normalizeSection_(requested));
      // Strip the param so a reload doesn't keep re-opening settings.
      const cleanUrl = window.location.pathname + window.location.hash;
      window.history.replaceState({}, '', cleanUrl);
    }
  }

  override disconnectedCallback(): void {
    this.clearCaptionButtonsCap_();
    if (window.customBrowserOpenNexusSettings === this.openSettingsFromHost_) {
      delete window.customBrowserOpenNexusSettings;
    }
    super.disconnectedCallback();
  }

  private normalizeSection_(section?: string): string {
    // Legacy alias from the C++ host: 'webapp' now lives under About.
    if (section === 'webapp') {
      return 'about';
    }
    if (section === 'mcp' || section === 'permissions' ||
        section === 'tools' || section === 'shortcuts' ||
        section === 'features' || section === 'plugins' ||
        section === 'system' || section === 'about') {
      return section;
    }
    return 'models';
  }

  // Forward the deep-link to the React guest (settings live there now). Posts
  // immediately AND once after a delay: a `?settings=` link on a fresh tab
  // races the guest boot, so the delayed retry catches the case where the
  // webview's contentWindow wasn't ready yet. The React receiver is
  // idempotent (same section), so the double-post is harmless.
  private forwardSettingsDeepLink_(section: string) {
    const post = () => {
      const view =
          this.shadowRoot?.querySelector<NexusViewElement>('nexus-view');
      view?.forwardSettingsDeepLink(section);
    };
    post();
    setTimeout(post, 2000);
  }

  protected override render() {
    return html`
      <div id="pane">
        <nexus-view
            @nexus-boot-welcome-closed=${this.onBootWelcomeClosed_}>
        </nexus-view>
      </div>
      <!-- Native OS caption controls, painted over the window's top-right
           (see :host styles). Sibling of #pane (not a child) so the
           #pane flex layout doesn't stretch it. -->
      <caption-button-container align-x="right"
          ?hidden=${this.captionButtonsHidden_}></caption-button-container>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'nexus-page': NexusPage;
  }

  interface Window {
    customBrowserOpenNexusSettings?: (section?: string) => void;
  }
}

customElements.define('nexus-page', NexusPage);
