// Copyright 2025 The Custom Browser Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Boot-time welcome overlay rendered inside <nexus-view> while the chrome://glic
// guest webview boots and the nexus_web React app hydrates. The same visual is
// displayed by the React side after legacy paint, but here it is painted by the
// parent WebUI immediately on first paint so the user sees one continuous
// welcome from open until the React guest reports "fully ready" via the
// nexus.ui.showHostLayout bridge action.

import {
  CrLitElement,
  css,
  html,
  type CSSResultGroup,
} from 'chrome://resources/lit/v3_0/lit.rollup.js';

import {getString} from './nexus_shared.js';

// Localized welcome copy. Ported from the guest's i18n catalogs
// (nexus_web src/browser/webview/i18n/messages/*) so the parent boot overlay
// speaks the same language the React shell will. The overlay paints BEFORE the
// guest loads, so it cannot read the guest's strings — it owns its own copy.
// Locale resolution mirrors nexus_web's `resolveLocale()` (BCP-47 prefix → a
// supported catalog, English fallback); both sides key off `navigator.language`
// (Chrome's UI language), so the parent and guest always agree.
type WelcomeLocale = 'en'|'zh'|'es'|'fr'|'de';

interface WelcomeCopy {
  greeting: string;
  subtitle: string;
}

const WELCOME_COPY: Record<WelcomeLocale, WelcomeCopy> = {
  en: {greeting: 'Hello there!', subtitle: 'How can I help you today?'},
  zh: {greeting: '你好！', subtitle: '有什么可以帮你的吗？'},
  es: {greeting: '¡Hola!', subtitle: '¿En qué puedo ayudarte hoy?'},
  fr: {
    greeting: 'Bonjour !',
    subtitle: 'Comment puis-je vous aider aujourd’hui ?',
  },
  de: {greeting: 'Hallo!', subtitle: 'Wie kann ich dir heute helfen?'},
};

function resolveWelcomeLocale(raw: string|null|undefined): WelcomeLocale {
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

export class NexusWelcomeElement extends CrLitElement {
  static get is() {
    return 'nexus-welcome';
  }

  static override styles: CSSResultGroup = css`
    :host {
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      width: 100%;
      height: 100%;
      padding: 24px;
      background:
          linear-gradient(180deg, #e2e8f0 0%, #f8fafc 45%, #ffffff 100%);
      color: #0f172a;
      font-family: ui-sans-serif, system-ui, sans-serif;
      overflow: hidden;
      pointer-events: none;
    }

    @keyframes nexus-loading-flash {
      0%, 100% { opacity: 0.25; }
      50%      { opacity: 1; }
    }

    .welcome-logo {
      width: 72px;
      height: 72px;
      display: block;
      margin-bottom: 24px;
      animation: nexus-loading-flash 1.4s ease-in-out infinite;
    }

    .welcome-text {
      text-align: center;
    }

    .welcome-title {
      font-size: clamp(1.5rem, 3.5vw, 2rem);
      font-weight: 600;
      line-height: 1.2;
      margin-bottom: 6px;
    }

    .welcome-subtitle {
      font-size: clamp(1rem, 2vw, 1.125rem);
      color: #475569;
      line-height: 1.4;
    }

    .visually-hidden {
      position: absolute;
      width: 1px;
      height: 1px;
      padding: 0;
      margin: -1px;
      overflow: hidden;
      clip: rect(0, 0, 0, 0);
      white-space: nowrap;
      border: 0;
    }
  `;

  protected override render() {
    const copy = WELCOME_COPY[resolveWelcomeLocale(navigator.language)];
    return html`
      <span class="visually-hidden" role="status" aria-busy="true">
        Loading ${getString('customBrowserProductName', 'Velloc')}
      </span>
      <img class="welcome-logo"
          srcset="nexus_welcome_logo.png 1x,
                  nexus_welcome_logo@2x.png 2x"
          src="nexus_welcome_logo.png"
          alt=""
          role="presentation">
      <div class="welcome-text">
        <div class="welcome-title">${copy.greeting}</div>
        <div class="welcome-subtitle">${copy.subtitle}</div>
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'nexus-welcome': NexusWelcomeElement;
  }
}

customElements.define(NexusWelcomeElement.is, NexusWelcomeElement);
