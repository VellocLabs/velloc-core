// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Recovery overlay in <nexus-view>, shown when the guest never acks the
// `nexus-init` handshake or fails to load. It lives in the host page so a bad
// `nexus.webapp_url` cannot disable it. The trigger is the missing ack, not a
// load error: IsAllowedCustomGuestUrl() checks only scheme/host, and a wrong
// origin's 404 is a successful load.

import {
  CrLitElement,
  css,
  html,
  type CSSResultGroup,
} from 'chrome://resources/lit/v3_0/lit.rollup.js';

import {getString} from './nexus_shared.js';

// Copy comes from custom_browser_strings.grd through loadTimeData
// (custom_browser_webui.cc, AddNexusStrings) — host-side, so it is there even
// though the guest, and with it the guest's i18n catalogs, never loaded.

export class NexusGuestRecoveryElement extends CrLitElement {
  static get is() {
    return 'nexus-guest-recovery';
  }

  static override styles: CSSResultGroup = css`
    :host {
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      width: 100%;
      height: 100%;
      /* Same reason as <nexus-welcome>: keep padding inside the 100% box or
       * the container's overflow:hidden clips the bottom. */
      box-sizing: border-box;
      padding: 24px;
      /* Same theme-following surface as <nexus-welcome> (see its comment). */
      background:
          linear-gradient(180deg,
              color-mix(in srgb, var(--text-primary) 10%, var(--app-active-tab-bg)) 0%,
              color-mix(in srgb, var(--text-primary) 3%, var(--app-active-tab-bg)) 45%,
              var(--app-active-tab-bg) 100%);
      color: var(--text-primary);
      font-family: ui-sans-serif, system-ui, sans-serif;
      overflow: hidden;
      /* The overlay body doubles as a window-drag handle (as the boot overlay
       * does), but the controls below opt out via -webkit-app-region:no-drag —
       * without that the buttons would be swallowed by the drag region and
       * become unclickable, which would defeat the entire point of this
       * element. */
      -webkit-app-region: drag;
      pointer-events: auto;
      position: relative;
    }

    .recovery-card {
      display: flex;
      flex-direction: column;
      align-items: center;
      gap: 12px;
      max-width: 420px;
      text-align: center;
    }

    .recovery-title {
      font-size: 16px;
      font-weight: 600;
      line-height: 1.4;
    }

    .recovery-body {
      font-size: 13px;
      line-height: 1.5;
      color: var(--text-secondary);
    }

    .recovery-url {
      font-size: 11px;
      line-height: 1.4;
      color: var(--text-disabled);
      word-break: break-all;
      max-width: 100%;
    }

    .recovery-url .label {
      font-weight: 600;
    }

    .recovery-actions {
      display: flex;
      gap: 8px;
      margin-top: 4px;
      -webkit-app-region: no-drag;
    }

    button {
      font: inherit;
      font-size: 13px;
      padding: 6px 14px;
      border-radius: 6px;
      cursor: pointer;
      -webkit-app-region: no-drag;
    }

    /* Inverted "filled" button: text colour as the fill, surface as the
     * label — the same relationship the old #0f172a-on-white pair had, now
     * theme-following. */
    button.primary {
      border: 1px solid transparent;
      background: var(--text-primary);
      color: var(--app-active-tab-bg);
    }

    button.primary:hover {
      background: color-mix(in srgb, var(--text-primary) 85%, var(--app-active-tab-bg));
    }

    button.secondary {
      border: 1px solid color-mix(in srgb, var(--text-primary) 25%, transparent);
      background: transparent;
      color: var(--text-primary);
    }

    button.secondary:hover {
      background: var(--control-hover);
    }

    button:focus-visible {
      outline: 2px solid var(--focus-ring);
      outline-offset: 2px;
    }
  `;

  // The guest URL that failed, shown verbatim so the user (or a bug report)
  // can see WHICH source was tried. Set by <nexus-view> via `.guestUrl=`.
  // Manual setter + requestUpdate(), matching <nexus-welcome> and the rest of
  // pages/nexus/ rather than Lit's reactive-property machinery.
  private guestUrl_ = '';
  // Set by <nexus-view> from the browser's connection state. Offline, the
  // runtime source is not to blame and resetting it would wipe a URL the user
  // may have pinned deliberately — so the offline state names the connection,
  // offers only Try again, and <nexus-view> retries by itself on `online`.
  private offline_ = false;

  get guestUrl(): string {
    return this.guestUrl_;
  }

  set guestUrl(value: string) {
    if (value === this.guestUrl_) {
      return;
    }
    this.guestUrl_ = value;
    this.requestUpdate();
  }

  get offline(): boolean {
    return this.offline_;
  }

  set offline(value: boolean) {
    if (value === this.offline_) {
      return;
    }
    this.offline_ = value;
    this.requestUpdate();
  }

  private onReset_() {
    this.dispatchEvent(new CustomEvent('nexus-guest-recovery-reset'));
  }

  private onRetry_() {
    this.dispatchEvent(new CustomEvent('nexus-guest-recovery-retry'));
  }

  protected override render() {
    if (this.offline_) {
      return html`
        <div class="recovery-card" id="recoveryCard" data-state="offline"
            role="alert">
          <div class="recovery-title">
            ${getString('customBrowserNexusGuestOfflineTitle')}
          </div>
          <div class="recovery-body">
            ${getString('customBrowserNexusGuestOfflineBody')}
          </div>
          <div class="recovery-actions">
            <button class="primary"
                id="retryRuntime"
                @click=${this.onRetry_}>
              ${getString('customBrowserNexusGuestRecoveryRetry')}
            </button>
          </div>
        </div>
      `;
    }
    return html`
      <div class="recovery-card" id="recoveryCard" data-state="failed"
          role="alert">
        <div class="recovery-title">
          ${getString('customBrowserNexusGuestRecoveryTitle')}
        </div>
        <div class="recovery-body">
          ${getString('customBrowserNexusGuestRecoveryBody')}
        </div>
        ${this.guestUrl_ ? html`
          <div class="recovery-url">
            <span class="label">
              ${getString('customBrowserNexusGuestRecoveryUrlLabel')}
            </span> ${this.guestUrl_}
          </div>
        ` : ''}
        <div class="recovery-actions">
          <button class="primary"
              id="resetRuntime"
              @click=${this.onReset_}>
            ${getString('customBrowserNexusGuestRecoveryReset')}
          </button>
          <button class="secondary"
              id="retryRuntime"
              @click=${this.onRetry_}>
            ${getString('customBrowserNexusGuestRecoveryRetry')}
          </button>
        </div>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'nexus-guest-recovery': NexusGuestRecoveryElement;
  }
}

customElements.define(NexusGuestRecoveryElement.is, NexusGuestRecoveryElement);
