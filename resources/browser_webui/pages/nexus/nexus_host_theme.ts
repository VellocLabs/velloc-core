// Host -> guest theme delivery: this WebUI page resolves colour roles from the
// live palette and posts them to the guest, which keeps its built-in palette
// when nothing arrives. Wire (additive, no interface bump, G-3):
//   { type: 'nexus-theme', nexus: true,
//     theme: { scheme: 'light'|'dark', colors: { <role>: 'rgb(r, g, b)' } } }
// Sent after the bridge bootstrap is forwarded and on every colour change; the
// guest drops any value that is not an `rgb()`/`rgba()` string.

import {COLORS_CSS_SELECTOR} from
    'chrome://resources/cr_components/color_change_listener/colors_css_updater.js';

export const NEXUS_THEME_MESSAGE_TYPE = 'nexus-theme';

/**
 * Colour roles the guest understands, each resolved from one Chrome colour
 * variable (chrome://theme/colors.css?sets=ui,chrome). Order is cosmetic.
 */
export const HOST_THEME_ROLES: ReadonlyArray<readonly [role: string, cssVar: string]> = [
  // Page ground. sys-surface: #ffffff light / #1f1f1f dark.
  ['surface', '--color-sys-surface'],
  // The toolbar / active-tab surface: the topbar and the composer paint from
  // it so they read as the same bar as Chrome's own. toolbar: #ffffff / #3c3c3c.
  ['toolbar', '--color-toolbar'],
  // Raised surfaces (cards, popovers). surface1: #f8fafd / #27282a.
  ['surfaceRaised', '--color-sys-surface1'],
  // Recessed/containers (muted, secondary, accent hover). surface3: #eff3fa / #333438.
  ['surfaceContainer', '--color-sys-surface3'],
  // The session sidebar, the composer box and the running-status bar all sit
  // on the page surface: one flat ground, the
  // panels told apart by their dividers, not by tint.
  ['sidebar', '--color-sys-surface'],
  ['base', '--color-sys-surface'],
  // Body text. sys-on-surface: #1f1f1f / #e3e3e3.
  ['textPrimary', '--color-sys-on-surface'],
  // Secondary text. cr-secondary-text-color.
  ['textSecondary', '--cr-secondary-text-color'],
  // Hairlines: the secondary TEXT colour, which the guest mixes to ~20% —
  // the same line the markdown tables always drew (user direction: one
  // line everywhere). cr-secondary-text-color: #474747 / #c7c7c7.
  ['outline', '--cr-secondary-text-color'],
  // The accent (buttons, active states) and text on it.
  ['primary', '--color-sys-primary'],
  ['onPrimary', '--color-sys-on-primary'],
  // Focus ring. cr-focus-outline-color.
  ['focusRing', '--cr-focus-outline-color'],
];

export interface HostThemePayload {
  scheme: 'light'|'dark';
  colors: Record<string, string>;
}

/** Resolve every role to the browser's computed `rgb()` string. Roles whose
 *  variable is undefined resolve to `rgba(0, 0, 0, 0)` and are omitted, so
 *  the guest keeps its fallback for that token instead of painting
 *  transparent. */
export function readHostTheme(doc: Document = document): HostThemePayload {
  const probe = doc.createElement('div');
  probe.style.position = 'absolute';
  probe.style.visibility = 'hidden';
  probe.style.pointerEvents = 'none';
  doc.body.appendChild(probe);
  const colors: Record<string, string> = {};
  try {
    for (const [role, cssVar] of HOST_THEME_ROLES) {
      probe.style.backgroundColor = '';
      probe.style.backgroundColor = `var(${cssVar})`;
      const value = getComputedStyle(probe).backgroundColor;
      if (value && value !== 'rgba(0, 0, 0, 0)' && value !== 'transparent') {
        colors[role] = value;
      }
    }
  } finally {
    probe.remove();
  }
  return {scheme: schemeOf(colors, doc), colors};
}

/** Light or dark, judged from the RESOLVED surface colour rather than this
 *  page's media query: the colours come straight from the browser's colour
 *  provider, while `prefers-color-scheme` on a WebUI page can be emulated
 *  (Playwright does so by default) and then disagrees with them. The media
 *  query is only the fallback when no surface resolved. */
function schemeOf(colors: Record<string, string>, doc: Document): 'light'|'dark' {
  const [r, g, b] = colors['surface']?.match(/[\d.]+/g)?.map(Number) ?? [];
  if (r !== undefined && g !== undefined && b !== undefined) {
    const lum = (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255;
    return lum < 0.5 ? 'dark' : 'light';
  }
  const dark = doc.defaultView?.matchMedia('(prefers-color-scheme: dark)').matches ?? false;
  return dark ? 'dark' : 'light';
}

type ThemeTarget = {
  contentWindow: WindowProxy|null;
};

/**
 * Post the current theme to the guest now and on every colour-provider
 * change. Returns a stop function; call it before starting a new sync for
 * the same webview (a re-handshake after a guest reload) so two senders
 * never race.
 */
export function startHostThemeSync(
    guest: ThemeTarget, guestOrigin: string,
    trace?: (message: string, details?: unknown) => void): () => void {
  let lastSent = '';
  const send = (force = false) => {
    const win = guest.contentWindow;
    if (!win) {
      return;
    }
    const theme = readHostTheme();
    const key = JSON.stringify(theme);
    if (!force && key === lastSent) {
      return;
    }
    try {
      win.postMessage(
          {type: NEXUS_THEME_MESSAGE_TYPE, nexus: true, theme}, guestOrigin);
      lastSent = key;
      trace?.('posted nexus-theme', {scheme: theme.scheme, roles: Object.keys(theme.colors).length});
    } catch (e) {
      trace?.('nexus-theme postMessage failed', {
        message: e instanceof Error ? e.message : String(e),
      });
    }
  };
  // A theme change is not one instant. The media query flips first, while
  // colors.css still holds the OLD palette (reading it then would hand the
  // guest the previous theme — the "one step behind" bug); the fresh
  // colors.css lands a little later. So every trigger re-samples for a
  // short window and posts only when the resolved values actually changed.
  // Idempotent by content, so the settle window can be generous.
  const SETTLE_SAMPLES_MS = [0, 60, 160, 320, 640, 1200, 2000];
  let settleTimers: number[] = [];
  const settle = () => {
    for (const t of settleTimers) clearTimeout(t);
    settleTimers = SETTLE_SAMPLES_MS.map(ms => setTimeout(() => send(), ms));
  };
  // ColorChangeUpdater (started in index.ts) refreshes the palette by
  // appending a NEW chrome://theme/colors.css <link> — to <body>, not
  // <head> — and removing the old one once the new one has loaded. Watch the
  // whole document for that link and re-sample when it finishes loading.
  const onMutation = (records: MutationRecord[]) => {
    for (const record of records) {
      for (const node of Array.from(record.addedNodes)) {
        if (node instanceof HTMLLinkElement && node.matches(COLORS_CSS_SELECTOR)) {
          node.addEventListener('load', settle, {once: true});
          settle();
        }
      }
    }
  };
  const observer = new MutationObserver(onMutation);
  observer.observe(document.documentElement, {childList: true, subtree: true});
  const mql = window.matchMedia('(prefers-color-scheme: dark)');
  mql.addEventListener('change', settle);
  send(true);
  return () => {
    for (const t of settleTimers) clearTimeout(t);
    observer.disconnect();
    mql.removeEventListener('change', settle);
  };
}
