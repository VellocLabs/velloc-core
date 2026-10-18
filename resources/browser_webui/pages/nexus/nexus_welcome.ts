// Copyright 2025 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

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

// Boot stages, in order. `starting`..`loading` are host-observed from the
// handshake; `rendering`..`finalizing` are real guest milestones (never
// timers), some broadcast by the SharedWorker. All feed one monotonic setter.
// There is no terminal "ready" stage: the overlay is torn down at that point.
export type LoadingStage = 'starting'|'connecting'|'linking'|'loading'|
    'rendering'|'sessions'|'connectingTools'|'restoring'|'opening'|
    'finalizing';

// Canonical stage order. <nexus-view> uses this to advance the footer
// MONOTONICALLY: the host handshake and the guest's `bootProgress` broadcasts
// feed the stage over two channels, and a later-arriving lower stage (or an
// unknown one from a newer guest) must never move the footer backwards.
export const LOADING_STAGE_ORDER: readonly LoadingStage[] = [
  'starting', 'connecting', 'linking', 'loading', 'rendering', 'sessions',
  'connectingTools', 'restoring', 'opening', 'finalizing',
];

interface WelcomeCopy {
  greeting: string;
  subtitle: string;
  // Footer client label; `{product}` is substituted with the product name.
  // Placement mirrors the guest's `settings.about.client` per locale.
  client: string;
  // Footer runtime line; `{date}` is substituted with the formatted build
  // time. Uses "built" (vs the About page's "Runtime ({date})") to make
  // explicit that the date is the runtime's build time.
  runtimeBuilt: string;
  // Footer live loading-status label per boot stage.
  loading: Record<LoadingStage, string>;
}

const WELCOME_COPY: Record<WelcomeLocale, WelcomeCopy> = {
  en: {
    greeting: 'Hello there!',
    subtitle: 'How can I help you today?',
    client: '{product} Client',
    runtimeBuilt: 'Runtime built {date}',
    loading: {
      starting: 'Starting up…',
      connecting: 'Connecting…',
      linking: 'Preparing…',
      loading: 'Loading tools…',
      rendering: 'Building interface…',
      sessions: 'Loading sessions…',
      connectingTools: 'Connecting tools…',
      restoring: 'Restoring conversation…',
      opening: 'Opening session…',
      finalizing: 'Almost ready…',
    },
  },
  zh: {
    greeting: '你好！',
    subtitle: '有什么可以帮你的吗？',
    client: '{product} 客户端',
    runtimeBuilt: '运行时构建于 {date}',
    loading: {
      starting: '正在启动…',
      connecting: '正在连接…',
      linking: '正在准备…',
      loading: '正在加载工具…',
      rendering: '正在构建界面…',
      sessions: '正在加载会话列表…',
      connectingTools: '正在连接工具…',
      restoring: '正在恢复对话…',
      opening: '正在打开会话…',
      finalizing: '即将就绪…',
    },
  },
  es: {
    greeting: '¡Hola!',
    subtitle: '¿En qué puedo ayudarte hoy?',
    client: 'Cliente {product}',
    runtimeBuilt: 'Runtime compilado el {date}',
    loading: {
      starting: 'Iniciando…',
      connecting: 'Conectando…',
      linking: 'Preparando…',
      loading: 'Cargando herramientas…',
      rendering: 'Construyendo la interfaz…',
      sessions: 'Cargando sesiones…',
      connectingTools: 'Conectando herramientas…',
      restoring: 'Restaurando la conversación…',
      opening: 'Abriendo sesión…',
      finalizing: 'Casi listo…',
    },
  },
  fr: {
    greeting: 'Bonjour !',
    subtitle: 'Comment puis-je vous aider aujourd’hui ?',
    client: 'Client {product}',
    runtimeBuilt: 'Runtime compilé le {date}',
    loading: {
      starting: 'Démarrage…',
      connecting: 'Connexion…',
      linking: 'Préparation…',
      loading: 'Chargement des outils…',
      rendering: 'Construction de l’interface…',
      sessions: 'Chargement des sessions…',
      connectingTools: 'Connexion des outils…',
      restoring: 'Restauration de la conversation…',
      opening: 'Ouverture de la session…',
      finalizing: 'Bientôt prêt…',
    },
  },
  de: {
    greeting: 'Hallo!',
    subtitle: 'Wie kann ich dir heute helfen?',
    client: '{product}-Client',
    runtimeBuilt: 'Runtime erstellt am {date}',
    loading: {
      starting: 'Wird gestartet…',
      connecting: 'Verbinden…',
      linking: 'Wird vorbereitet…',
      loading: 'Tools werden geladen…',
      rendering: 'Oberfläche wird erstellt…',
      sessions: 'Sitzungen werden geladen…',
      connectingTools: 'Tools werden verbunden…',
      restoring: 'Unterhaltung wird wiederhergestellt…',
      opening: 'Sitzung wird geöffnet…',
      finalizing: 'Fast fertig…',
    },
  },
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

// The welcome locale a host strings pak locale ("zh-CN", "en-US") is written
// in, or null when the welcome copy has no catalog for that language.
export function welcomeLocaleOfHostStrings(raw: string|null|undefined):
    WelcomeLocale|null {
  const lang = (raw ?? '').toLowerCase();
  if (lang === 'zh-cn') {
    return 'zh';
  }
  const primary = lang.split('-')[0];
  return primary === 'en' || primary === 'es' || primary === 'fr' ||
          primary === 'de' ?
      primary :
      null;
}

// A host string joins the welcome copy only when both are in one language.
export function hostStringsMatchWelcome(
    hostLocale: string|null|undefined,
    browserLanguage: string|null|undefined): boolean {
  const host = welcomeLocaleOfHostStrings(hostLocale);
  return host !== null && host === resolveWelcomeLocale(browserLanguage);
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
      /* Keep padding INSIDE the 100% box. Without this the default
       * content-box sizing makes the host 48px (2 x 24px) taller than its
       * container, and the container's overflow:hidden clips the bottom —
       * which pushed the bottom-anchored .welcome-footer off-screen. */
      box-sizing: border-box;
      padding: 24px;
      /* Theme-following surface: the host page aliases Chrome's own colour
       * tokens (index.html: --app-active-tab-bg = the ACTIVE tab's background, i.e.
       * the toolbar/content surface, white in light; NOT --app-bg, the
       * tab-strip ground, which is blue-grey in light and read as a dirty
       * splash —, --text-* =
       * the cr-*-text colours), so this overlay tracks Appearance > Mode and
       * the user's colour theme exactly like the tab strip beside it. The
       * light gradient (slate-200 → white) is reproduced as "a little text
       * colour mixed into the surface at the top, fading to the surface",
       * which reads the same in light and inverts correctly in dark. */
      background:
          linear-gradient(180deg,
              color-mix(in srgb, var(--text-primary) 10%, var(--app-active-tab-bg)) 0%,
              color-mix(in srgb, var(--text-primary) 3%, var(--app-active-tab-bg)) 45%,
              var(--app-active-tab-bg) 100%);
      color: var(--text-primary);
      font-family: ui-sans-serif, system-ui, sans-serif;
      overflow: hidden;
      /* The whole boot overlay is a window-drag handle. The parent WebUI view
       * (custom_browser_webui_view.cc) supports web draggable regions, so
       * -webkit-app-region:drag here makes dragging anywhere on the welcome
       * page move the window. pointer-events:auto (was :none) makes the
       * overlay the interaction surface — click-through to the still-booting
       * guest isn't needed while the overlay is up. */
      -webkit-app-region: drag;
      pointer-events: auto;
      /* Containing block for the absolutely positioned footer. The parent's
       * .welcome-overlay rule also sets position:absolute (higher
       * specificity); either way the footer anchors to this host. */
      position: relative;
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
      color: var(--text-secondary);
      line-height: 1.4;
      max-width: 440px;
      margin: 0 auto;
      text-wrap: balance;
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

    /* Subtle version/build metadata pinned to the bottom center. Transparent,
     * no card/border/shadow; label and value share one muted color. */
    .welcome-footer {
      position: absolute;
      bottom: 28px;
      left: 0;
      right: 0;
      text-align: center;
      font-size: 12px;
      font-weight: 400;
      line-height: 18px;
      color: var(--text-disabled);
      white-space: nowrap;
    }

    .welcome-footer .sep {
      color: color-mix(in srgb, var(--text-disabled) 55%, transparent);
    }
  `;

  // ISO build time, set via the `.buildTime=` binding once build info arrives;
  // empty until then (the footer shows only client + version). A plain field
  // whose setter calls requestUpdate(), not a reactive property.
  private buildTime_ = '';

  get buildTime(): string {
    return this.buildTime_;
  }

  set buildTime(value: string) {
    if (value === this.buildTime_) {
      return;
    }
    this.buildTime_ = value;
    this.requestUpdate();
  }

  // Current boot stage, set by <nexus-view> via the `.loadingStage=` binding
  // as the host handshake progresses. Same manual-setter pattern as
  // `buildTime`; rendered as the live trailing footer segment.
  private loadingStage_: LoadingStage = 'starting';

  get loadingStage(): LoadingStage {
    return this.loadingStage_;
  }

  set loadingStage(value: LoadingStage) {
    if (value === this.loadingStage_) {
      return;
    }
    this.loadingStage_ = value;
    this.requestUpdate();
  }

  // Set by <nexus-view> once a first run has been on the welcome for a while:
  // the subtitle then tells the user the first start takes longer.
  private firstRunNote_ = false;

  get firstRunNote(): boolean {
    return this.firstRunNote_;
  }

  set firstRunNote(value: boolean) {
    if (value === this.firstRunNote_) {
      return;
    }
    this.firstRunNote_ = value;
    this.requestUpdate();
  }

  // Format an ISO build timestamp for the footer, matching the guest's
  // formatBuildTime() options so the boot overlay and the About page render
  // the same "Jul 11, 2026, 5:30 PM" form. Empty for missing/invalid input.
  private formatBuildTime_(iso: string, locale: string): string {
    if (!iso) {
      return '';
    }
    const d = new Date(iso);
    if (Number.isNaN(d.getTime())) {
      return '';
    }
    return d.toLocaleString(locale, {dateStyle: 'medium', timeStyle: 'short'});
  }

  protected override render() {
    const locale = navigator.language;
    const copy = WELCOME_COPY[resolveWelcomeLocale(locale)];
    const productName = getString('customBrowserProductName', 'Velloc');
    const version = getString('customBrowserProductVersion', '');
    // e.g. "Velloc Client 1.149.1.0" (client-label placement is localized).
    const clientText =
        copy.client.replace('{product}', productName) +
        (version ? ` ${version}` : '');
    // Async: empty until the guest pushes the build time.
    const buildTime = this.formatBuildTime_(this.buildTime, locale);
    const runtimeText =
        buildTime ? copy.runtimeBuilt.replace('{date}', buildTime) : '';
    const loadingText = copy.loading[this.loadingStage_] ?? '';
    const subtitle = this.firstRunNote_ &&
            hostStringsMatchWelcome(
                getString('customBrowserNexusHostStringsLocale'), locale) ?
        getString('customBrowserNexusWelcomeFirstRunPatience', copy.subtitle) :
        copy.subtitle;
    // Footer format: {client version} · {runtime} · {loading status}. The
    // runtime segment is omitted until the async build time arrives; the
    // segments are dot-separated with a lighter middot.
    const segments = [clientText, runtimeText, loadingText].filter(Boolean);
    return html`
      <span class="visually-hidden" role="status" aria-busy="true">
        Loading ${productName}
      </span>
      <img class="welcome-logo"
          srcset="nexus_welcome_logo.png 1x,
                  nexus_welcome_logo@2x.png 2x"
          src="nexus_welcome_logo.png"
          alt=""
          role="presentation">
      <div class="welcome-text">
        <div class="welcome-title">${copy.greeting}</div>
        <div class="welcome-subtitle" id="welcomeSubtitle"
            data-first-run-note=${this.firstRunNote_ ? 'shown' : 'hidden'}
            aria-live="polite">${subtitle}</div>
      </div>
      <div class="welcome-footer">
        ${segments.map((seg, i) => html`${
            i > 0 ? html`<span class="sep"> · </span>` : ''}${seg}`)}
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
