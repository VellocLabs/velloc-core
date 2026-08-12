// Copyright 2026 The Custom Browser Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Recovery overlay rendered inside <nexus-view> when the guest webview never
// completes the `nexus-init` handshake (nexus_runtime_bootstrap.ts), or fails
// to load outright.
//
// Why this exists: the guest URL is a profile pref (`nexus.webapp_url`) and
// the ONLY editor for it used to live in the guest's own Settings · About
// page — inside the very app that fails to load. A bad URL therefore bricked
// the panel with no in-product way back: recovery meant DevTools on
// chrome://custom.top-chrome or hand-editing the profile's Preferences JSON.
// This overlay is the way back, and it lives in the HOST page so it is
// unaffected by whatever the guest origin does.
//
// It matters most for a wrong-but-VALID URL. `IsAllowedCustomGuestUrl()`
// (nexus_guest_config.cc) only checks scheme/host, and the binary pins its
// compiled-in `/v<N>/` path (AppendNexusInterfaceVersionToBase), so pointing
// at an origin that does not serve this interface version yields a plain HTTP
// 404. A 404 is a SUCCESSFUL load — `loadabort` never fires and the webview
// happily renders the error body — which is exactly why the trigger for this
// overlay is the handshake never being acked, not a load error.

import {
  CrLitElement,
  css,
  html,
  type CSSResultGroup,
} from 'chrome://resources/lit/v3_0/lit.rollup.js';

import {getString} from './nexus_shared.js';

// Localized copy, owned by this element for the same reason <nexus-welcome>
// owns its own: the overlay paints when the guest is unreachable, so it cannot
// read the guest's i18n catalogs. Locale resolution mirrors nexus_web's
// `resolveLocale()` (BCP-47 prefix → supported catalog, English fallback).
type RecoveryLocale = 'en'|'zh'|'es'|'fr'|'de';

interface RecoveryCopy {
  title: string;
  body: string;
  urlLabel: string;
  reset: string;
  retry: string;
}

const RECOVERY_COPY: Record<RecoveryLocale, RecoveryCopy> = {
  en: {
    title: 'The {product} runtime didn’t load',
    body:
        'The panel could not start its runtime. This usually means the ' +
        'configured runtime source is unreachable, or does not serve the ' +
        'version this build requires.',
    urlLabel: 'Tried',
    reset: 'Restore default runtime',
    retry: 'Try again',
  },
  zh: {
    title: '{product} 运行时未能加载',
    body:
        '面板无法启动运行时。通常是配置的运行时来源无法访问，或它不提供当前' +
        '版本所需的接口版本。',
    urlLabel: '已尝试',
    reset: '恢复默认运行时',
    retry: '重试',
  },
  es: {
    title: 'No se pudo cargar el runtime de {product}',
    body:
        'El panel no pudo iniciar su runtime. Normalmente significa que el ' +
        'origen configurado no es accesible o no sirve la versión que ' +
        'requiere esta compilación.',
    urlLabel: 'Se intentó',
    reset: 'Restaurar runtime predeterminado',
    retry: 'Reintentar',
  },
  fr: {
    title: 'Le runtime {product} n’a pas pu être chargé',
    body:
        'Le panneau n’a pas pu démarrer son runtime. Cela signifie ' +
        'généralement que la source configurée est inaccessible ou qu’elle ' +
        'ne fournit pas la version requise par cette build.',
    urlLabel: 'Tentative',
    reset: 'Restaurer le runtime par défaut',
    retry: 'Réessayer',
  },
  de: {
    title: '{product}-Runtime konnte nicht geladen werden',
    body:
        'Das Panel konnte seine Runtime nicht starten. Meist ist die ' +
        'konfigurierte Quelle nicht erreichbar oder liefert nicht die von ' +
        'diesem Build benötigte Version.',
    urlLabel: 'Versucht',
    reset: 'Standard-Runtime wiederherstellen',
    retry: 'Erneut versuchen',
  },
};

function resolveRecoveryLocale(raw: string|null|undefined): RecoveryLocale {
  const lang = (raw ?? '').toLowerCase();
  if (lang.startsWith('zh')) {
    return 'zh';
  }
  if (lang.startsWith('es')) {
    return 'es';
  }
  if (lang.startsWith('fr')) {
    return 'fr';
  }
  if (lang.startsWith('de')) {
    return 'de';
  }
  return 'en';
}

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
      background:
          linear-gradient(180deg, #e2e8f0 0%, #f8fafc 45%, #ffffff 100%);
      color: #0f172a;
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
      color: #475569;
    }

    .recovery-url {
      font-size: 11px;
      line-height: 1.4;
      color: #64748b;
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

    button.primary {
      border: 1px solid transparent;
      background: #0f172a;
      color: #f8fafc;
    }

    button.primary:hover {
      background: #1e293b;
    }

    button.secondary {
      border: 1px solid #cbd5e1;
      background: transparent;
      color: #0f172a;
    }

    button.secondary:hover {
      background: #e2e8f0;
    }

    button:focus-visible {
      outline: 2px solid #2563eb;
      outline-offset: 2px;
    }
  `;

  // The guest URL that failed, shown verbatim so the user (or a bug report)
  // can see WHICH source was tried. Set by <nexus-view> via `.guestUrl=`.
  // Manual setter + requestUpdate(), matching <nexus-welcome> and the rest of
  // pages/nexus/ rather than Lit's reactive-property machinery.
  private guestUrl_ = '';

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

  private onReset_() {
    this.dispatchEvent(new CustomEvent('nexus-guest-recovery-reset'));
  }

  private onRetry_() {
    this.dispatchEvent(new CustomEvent('nexus-guest-recovery-retry'));
  }

  protected override render() {
    const copy = RECOVERY_COPY[resolveRecoveryLocale(navigator.language)];
    const productName = getString('customBrowserProductName', 'Velloc');
    const title = copy.title.replace('{product}', productName);
    const body = copy.body.replace('{product}', productName);
    return html`
      <div class="recovery-card" role="alert">
        <div class="recovery-title">${title}</div>
        <div class="recovery-body">${body}</div>
        ${this.guestUrl_ ? html`
          <div class="recovery-url">
            <span class="label">${copy.urlLabel}:</span> ${this.guestUrl_}
          </div>
        ` : ''}
        <div class="recovery-actions">
          <button class="primary"
              id="resetRuntime"
              @click=${this.onReset_}>${copy.reset}</button>
          <button class="secondary"
              id="retryRuntime"
              @click=${this.onRetry_}>${copy.retry}</button>
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
