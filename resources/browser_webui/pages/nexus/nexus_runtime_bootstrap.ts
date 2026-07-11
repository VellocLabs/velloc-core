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
  // Forward the centralized product name (custom_browser/common/
  // branding_constants.h -> kCustomBrowserProductName, surfaced via
  // loadTimeData in ui/web_ui/custom_browser_webui.cc) to the nexus_web
  // guest. The guest reads it in src/glic/main.tsx to brand the React
  // shell, so a single rebrand in branding_constants.h flows end-to-end.
  const productName = loadTimeData.valueExists('customBrowserProductName') ?
      loadTimeData.getString('customBrowserProductName') :
      '';
  if (productName) {
    url.searchParams.set('product', productName);
  }
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
    webview: BootstrapWebView, guestOrigin: string): Promise<void> {
  return new Promise((resolve, reject) => {
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
            { type: 'nexus-init', nexus: true }, guestOrigin);
      } catch (e) {
        debugTrace('nexus-init postMessage failed', {
          message: e instanceof Error ? e.message : String(e),
        });
      }
    };

    window.addEventListener('message', onMessage);
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
    guestOrigin: string): Promise<void> {
  return new Promise((resolve, reject) => {
    let timeout: ReturnType<typeof setTimeout> | null = null;

    const cleanup = () => {
      if (timeout) {
        clearTimeout(timeout);
        timeout = null;
      }
      window.removeEventListener('message', onMessage);
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
    timeout = setTimeout(() => {
      cleanup();
      reject(new Error(`nexus-bridge-bootstrap timed out after ${
          BRIDGE_BOOTSTRAP_TIMEOUT_MS}ms`));
    }, BRIDGE_BOOTSTRAP_TIMEOUT_MS);
  });
}

export async function bootstrapNexusGuest(
    webview: HTMLElement | null): Promise<void> {
  if (!isBootstrapWebView(webview)) {
    throw new Error(
        'bootstrapNexusGuest requires a <webview> element; got ' +
        (webview ? webview.tagName : 'null'));
  }

  const guestUrl = buildGuestUrl();
  const guestOrigin = originOf(guestUrl);
  debugTrace('bootstrapping nexus guest', {
    src: guestUrl.toString(),
    origin: guestOrigin,
  });

  webview.src = guestUrl.toString();

  await waitForGuestAck(webview, guestOrigin);

  const remote = NexusBridgeBootstrap.getRemote();
  remote.guestReady();
  debugTrace('called NexusBridgeBootstrap.guestReady()');

  await waitForBootstrapAndForward(webview, guestOrigin);
  debugTrace('bootstrap complete; runtime ports are owned by the webview');
}
