// Bootstrap controller for the Nexus guest <webview> hosted directly inside
// the CustomBrowserWebUI page (chrome://custom.top-chrome).
//
// Replaces the chrome://glic-based intermediate page that previously owned
// the bootstrap dance (`src/custom_browser/resources/patch_webui/glic/
// nexus_boot.ts`). The flow is the same in shape:
//
//   1. Set webview.src to the resolved guest URL (loadTimeData).
//   2. Send periodic `{type: 'nexus-init', nexus: true}` to the guest until
//      the guest replies with `{type: 'nexus-init-ack'}`.
//   3. Call `NexusBridgeBootstrap.getRemote().guestReady()`. The browser
//      posts a `nexus-bridge-bootstrap` message + 2 transferred MessagePorts
//      back into this page via MessagePortProvider::PostMessageToFrame.
//   4. Forward the message + ports to the guest webview.
//
// Once the guest receives the ports, its SharedWorker connects to the
// browser-process `NexusBridgePortService` directly — every subsequent
// `glic.executeHostedTool` call goes through that direct port instead of
// through this page.
//
// The bootstrap binding is registered by
// `src/custom_browser/features/patch_webui/custom_browser_hooks.cc`'s
// `BindNexusBridgeBootstrapForCustomBrowser` (which still serves both
// chrome://glic and chrome://custom.top-chrome during the transition).

import {NexusBridgeBootstrap} from
    '../../nexus_bridge_bootstrap.mojom-webui.js';
import {loadTimeData} from 'chrome://resources/js/load_time_data.js';

import type {LoadingStage} from './nexus_welcome.js';
import {startHostThemeSync} from './nexus_host_theme.js';
import {panelId} from './nexus_panel_id.js';
import {invalidateShellClient} from './nexus_shell_client.js';

// Reports host-side handshake progress to the boot-welcome overlay's footer.
// Same page/process as the overlay, so this is a direct callback — no bridge.
export type BootStatusReporter = (stage: LoadingStage) => void;

// Reports a terminal PRE-ACK handshake failure to the host, which surfaces the
// <nexus-guest-recovery> overlay.
//
// Scope is deliberately narrow, because the overlay blames the guest URL and
// offers to reset the pref: this fires only when the document at guestUrl never
// answered `nexus-init`. It does NOT fire once the guest has acked — a
// post-ack bridge-bootstrap failure is host-side and says nothing about the
// URL (see runHandshake). Nor does it fire for the aborted case (a newer guest
// load superseded this handshake), so a normal guest reload never triggers
// recovery.
//
// This exists because the failure is otherwise invisible. `runHandshake` is a
// detached async IIFE with its own try/catch, and `bootstrapNexusGuest`
// resolves as soon as it assigns `guest.src` — so the promise its caller
// awaits CANNOT reject on a handshake failure, and the failure used to end its
// life as a single debugTrace line while the panel sat blank.
export type BootFailureReporter = (detail: {
  reason: string,
  message: string,
  guestUrl: string,
}) => void;

const NEXUS_INIT_PING_INTERVAL_MS = 100;
const NEXUS_INIT_TIMEOUT_MS = 30_000;
const BRIDGE_BOOTSTRAP_TIMEOUT_MS = 30_000;

function debugTrace(message: string, details?: unknown): void {
  if (details === undefined) {
    console.info(`[NexusTrace][HostBootstrap] ${message}`);
    return;
  }
  console.info(`[NexusTrace][HostBootstrap] ${message}`, details);
}

function buildGuestUrl(): URL {
  const raw = loadTimeData.getString('customBrowserNexusGuestUrl') ||
              loadTimeData.getString('customBrowserNexusWebappDefaultUrl');
  const url = new URL(raw);
  url.searchParams.set('nexus', '1');
  // The visible product name is no longer forwarded as a `?product=` query
  // param. The guest reads it from its own build-time `VITE_PRODUCT_NAME`
  // (nexus_web/src/branding/productName.ts), unifying all branding on the
  // build-time path. (The boot welcome overlay still uses the loadTimeData
  // `customBrowserProductName` directly — see nexus_welcome.ts.)
  return url;
}

function originOf(url: URL): string {
  return `${url.protocol}//${url.host}`;
}

interface BootstrapWebView extends HTMLElement {
  src: string;
  contentWindow: WindowProxy | null;
}

function isBootstrapWebView(el: HTMLElement | null): el is BootstrapWebView {
  return !!el && el.tagName.toLowerCase() === 'webview';
}

// Repeatedly post `nexus-init` to the webview's contentWindow until we
// receive a `nexus-init-ack` reply. Inside a <webview> the guest cannot use
// `window.parent`, so it relies on the `event.source` from this ping to
// learn the host window.
function waitForGuestAck(
    webview: BootstrapWebView, guestOrigin: string,
    signal?: AbortSignal): Promise<void> {
  return new Promise((resolve, reject) => {
    if (signal?.aborted) {
      reject(new Error('nexus-init handshake aborted'));
      return;
    }
    let pingTimer: ReturnType<typeof setInterval> | null = null;
    let timeout: ReturnType<typeof setTimeout> | null = null;

    const cleanup = () => {
      if (pingTimer) {
        clearInterval(pingTimer);
        pingTimer = null;
      }
      if (timeout) {
        clearTimeout(timeout);
        timeout = null;
      }
      window.removeEventListener('message', onMessage);
      signal?.removeEventListener('abort', onAbort);
    };

    // A newer guest (re)load superseded this handshake — stop pinging and
    // tear down so we don't leave a stale ping loop / listener racing the
    // fresh handshake for the new guest's messages.
    const onAbort = () => {
      cleanup();
      reject(new Error('nexus-init handshake aborted'));
    };

    const onMessage = (event: MessageEvent) => {
      const data = event.data as Record<string, unknown> | null;
      if (data?.['type'] !== 'nexus-init-ack') {
        return;
      }
      cleanup();
      debugTrace('received nexus-init-ack', {origin: event.origin});
      resolve();
    };

    const sendPing = () => {
      if (!webview.contentWindow) {
        return;
      }
      try {
        webview.contentWindow.postMessage(
            { type: 'nexus-init', nexus: true, panelId: panelId() },
            guestOrigin);
      } catch (e) {
        debugTrace('nexus-init postMessage failed', {
          message: e instanceof Error ? e.message : String(e),
        });
      }
    };

    window.addEventListener('message', onMessage);
    signal?.addEventListener('abort', onAbort);
    pingTimer = setInterval(sendPing, NEXUS_INIT_PING_INTERVAL_MS);
    sendPing();

    timeout = setTimeout(() => {
      cleanup();
      reject(new Error(
          `nexus-init handshake timed out after ${NEXUS_INIT_TIMEOUT_MS}ms`));
    }, NEXUS_INIT_TIMEOUT_MS);
  });
}

// After we call `bootstrap.guestReady()`, the browser posts a
// `nexus-bridge-bootstrap` message + 2 transferred MessagePorts back to
// THIS window via `content::MessagePortProvider::PostMessageToFrame`. We
// forward both ports unchanged into the guest webview.
function waitForBootstrapAndForward(
    webview: BootstrapWebView,
    guestOrigin: string,
    signal?: AbortSignal): Promise<void> {
  return new Promise((resolve, reject) => {
    if (signal?.aborted) {
      reject(new Error('nexus-bridge-bootstrap aborted'));
      return;
    }
    let timeout: ReturnType<typeof setTimeout> | null = null;

    const cleanup = () => {
      if (timeout) {
        clearTimeout(timeout);
        timeout = null;
      }
      window.removeEventListener('message', onMessage);
      signal?.removeEventListener('abort', onAbort);
    };

    const onAbort = () => {
      cleanup();
      reject(new Error('nexus-bridge-bootstrap aborted'));
    };

    // Native posts the bootstrap message via
    // `content::MessagePortProvider::PostMessageToFrame`, which delivers
    // the JSON-serialized payload as a STRING (event.data), not as a
    // parsed object. Parse it here so we can dispatch on `type` / `nexus`
    // and pull out the manifest.
    const parseBootstrapPayload = (raw: unknown):
        Record<string, unknown> | null => {
      if (typeof raw === 'string') {
        try {
          return parseBootstrapPayload(JSON.parse(raw));
        } catch {
          return null;
        }
      }
      if (!raw || typeof raw !== 'object') {
        return null;
      }
      const obj = raw as Record<string, unknown>;
      if (obj['type'] === 'nexus-bridge-bootstrap' && obj['nexus'] === true) {
        return obj;
      }
      return null;
    };

    const onMessage = (event: MessageEvent) => {
      const data = parseBootstrapPayload(event.data);
      if (!data) {
        return;
      }
      if (!event.ports || event.ports.length < 2) {
        debugTrace('bridge bootstrap missing ports', {
          portCount: event.ports?.length ?? 0,
        });
        return;
      }
      cleanup();
      debugTrace('forwarding nexus-bridge-bootstrap to webview', {
        portCount: event.ports.length,
      });
      const manifest = data['manifest'];
      try {
        webview.contentWindow?.postMessage(
            { type: 'nexus-bridge-bootstrap', nexus: true, manifest },
            guestOrigin,
            Array.from(event.ports));
      } catch (e) {
        reject(new Error(
            `forwarding nexus-bridge-bootstrap failed: ${
                e instanceof Error ? e.message : String(e)}`));
        return;
      }
      resolve();
    };

    window.addEventListener('message', onMessage);
    signal?.addEventListener('abort', onAbort);
    timeout = setTimeout(() => {
      cleanup();
      reject(new Error(`nexus-bridge-bootstrap timed out after ${
          BRIDGE_BOOTSTRAP_TIMEOUT_MS}ms`));
    }, BRIDGE_BOOTSTRAP_TIMEOUT_MS);
  });
}

// One theme sync per webview: a re-handshake (guest reload) replaces the
// previous sender instead of stacking a second one.
const themeSyncStops = new WeakMap<HTMLElement, () => void>();

export async function bootstrapNexusGuest(
    webview: HTMLElement | null,
    onStatus?: BootStatusReporter,
    onFailure?: BootFailureReporter): Promise<void> {
  if (!isBootstrapWebView(webview)) {
    throw new Error(
        'bootstrapNexusGuest requires a <webview> element; got ' +
        (webview ? webview.tagName : 'null'));
  }
  const reportStatus = (stage: LoadingStage) => {
    try {
      onStatus?.(stage);
    } catch {
      // Status is cosmetic; never let a reporter throw break the handshake.
    }
  };

  // Capture the narrowed element in a const so the BootstrapWebView
  // narrowing survives into the closures below (a mutable parameter widens
  // back to `HTMLElement | null` inside nested functions).
  const guest: BootstrapWebView = webview;

  const guestUrl = buildGuestUrl();
  const guestOrigin = originOf(guestUrl);

  const reportFailure = (detail: {reason: string, message: string}) => {
    try {
      onFailure?.({...detail, guestUrl: guestUrl.toString()});
    } catch {
      // Recovery UI is best-effort; never let a reporter throw escape into
      // the handshake's catch block and mask the original failure.
    }
  };
  debugTrace('bootstrapping nexus guest', {
    src: guestUrl.toString(),
    origin: guestOrigin,
  });

  // The handshake must run on EVERY guest (re)load, not just the first.
  //
  // A guest reload (Vite full reload on `nexus_web` update, an in-app
  // `location.reload()`, or any re-navigation of the <webview>) destroys
  // the guest's JS state — including the host-window reference it captured
  // from the first `nexus-init`. The fresh guest re-enters
  // `waitForNexusHostInit()` and sits there forever unless the host pings
  // `nexus-init` again. The original one-shot bootstrap only ran from
  // `<nexus-view>.firstUpdated()`, so after a reload the panel stayed
  // blank. We now re-drive the full handshake on each guest `contentload`.
  //
  // The downstream layers already tolerate a repeat:
  //   • NexusBridgeBootstrapHost::GuestReady() reuses its
  //     NexusBridgePortService and CreatePortPair() resets + rebinds to the
  //     new guest's port (the old, now-dead port is closed).
  //   • SharedWorkerSessionManager.addBridgePort('direct') disposes the old
  //     DirectPortBridgeTransport and installs the new workerPort.
  let activeHandshake: AbortController | null = null;

  const runHandshake = (reason: string) => {
    // Supersede any in-flight handshake from a previous (now-stale) load so
    // we never leave two ping loops / message listeners fighting over the
    // new guest's messages.
    activeHandshake?.abort();
    const controller = new AbortController();
    activeHandshake = controller;
    const {signal} = controller;
    debugTrace('starting nexus handshake', {reason});

    void (async () => {
      // PRE-ACK. Deliberately its own try/catch, NOT merged with the post-ack
      // block below: only a failure here says anything about the guest URL.
      // waitForGuestAck pings every 100 ms for 30 s, so reaching its catch
      // means the document at guestUrl never answered `nexus-init` — a 404
      // body, a non-Nexus page, or a guest too broken to run its entry. That,
      // and only that, is what the recovery overlay is allowed to blame the
      // URL for and offer to reset the pref over.
      try {
        await waitForGuestAck(guest, guestOrigin, signal);
      } catch (e) {
        if (signal.aborted) {
          // Expected: a newer guest load superseded this handshake.
          return;
        }
        const message = e instanceof Error ? e.message : String(e);
        debugTrace('nexus-init handshake failed', {reason, message});
        // Terminal for THIS load. Nothing clears the overlay afterwards —
        // both of its buttons go through location.reload(), which restarts
        // the page and this whole controller from scratch.
        reportFailure({reason, message});
        return;
      }

      // POST-ACK. The guest loaded and answered `nexus-init`, so the guest URL
      // is proven good. Everything from here is host-side — the Mojo
      // guestReady() call and the browser's port-forwarding postMessage — and
      // a failure is the "Boot Failed / real bridge host not attached" class
      // (see G-1), NOT a bad runtime source. Reporting it
      // as one would show copy blaming the URL and, worse, offer a primary
      // button that wipes a `nexus.webapp_url` the user may have pinned on
      // purpose — while leaving the panel just as dead, because the URL was
      // never the problem. So this path stays diagnostic-only.
      try {
        // Guest acked the handshake; mint + forward the bridge ports next.
        reportStatus('linking');
        NexusBridgeBootstrap.getRemote().guestReady();
        debugTrace('called NexusBridgeBootstrap.guestReady()', {reason});
        await waitForBootstrapAndForward(guest, guestOrigin, signal);
        // Ports handed to the webview; the guest now boots its own runtime.
        reportStatus('loading');
        // Live theme: resolved Chrome colours, now and on every change. The
        // guest's listener is installed at its module entry, so nothing sent
        // after the ack can be missed. Additive message: an older guest
        // ignores it and keeps its built-in palette.
        themeSyncStops.get(guest)?.();
        themeSyncStops.set(
            guest, startHostThemeSync(guest, guestOrigin, debugTrace));
        debugTrace('bootstrap complete; runtime ports owned by the webview', {
          reason,
        });
      } catch (e) {
        if (signal.aborted) {
          return;
        }
        const message = e instanceof Error ? e.message : String(e);
        debugTrace('nexus bridge bootstrap failed', {reason, message});
      }
    })();
  };

  // `contentload` fires once per guest document load — the initial
  // navigation we trigger below AND every subsequent reload. Attach the
  // listener before setting `src` so the very first load is captured.
  guest.addEventListener('contentload', () => {
    // The panel's own runtime port died with the old guest document; a
    // connection still marked live would ignore the new guest's beacon.
    invalidateShellClient();
    runHandshake('contentload');
  });

  // About to load the guest page + run the handshake.
  reportStatus('connecting');
  guest.src = guestUrl.toString();
}
