/**
 * Outer-WebUI client for the SharedWorker bridge.
 *
 * The Chromium WebUI settings pages (chrome://...) live OUTSIDE the
 * nexus_web bundle — they can't import from `nexus_web/src/` directly
 * because the two are separate Vite builds. This module is the
 * intentionally tiny, self-contained client that gives any WebUI page a
 * direct line to the nexus_web SharedWorker.
 *
 * Handshake (one-time, per connection):
 *   1. WebUI creates a `MessageChannel()`.
 *   2. WebUI posts `{type:'nexus-webui-bridge-attach', nexus:true}` to
 *      the nexus_web webview's `contentWindow`, transferring `port2`.
 *   3. nexus_web's page thread (`webUiBridgeBootstrap.ts`) forwards
 *      that port to the SharedWorker via `bridge.attachWebUiPort`.
 *   4. SharedWorker (`addWebUiBridgePort`) stores the port and starts
 *      dispatching requests on it.
 *
 * Steady state:
 *   - `transport.request({route, payload})` resolves with a typed
 *     `{ok, result|error}` response.
 *   - `transport.subscribeEvents(handler)` receives worker → WebUI
 *     push events (e.g. "features changed in another tab").
 *
 * Wire envelope mirrors `DirectPortBridgeTransport` and the nexus_web
 * `MessagePortBridgeTransport`: `{claudeBridge: true, direction, …}`.
 * Discriminated by the `direction` field. Re-implemented here (instead
 * of importing) so the WebUI bundle stays standalone.
 *
 * Many-to-one: every call to `connectNexusWebUiBridge()` creates a new
 * `MessageChannel` and a new attached port; the SharedWorker handles
 * each as an independent peer. The settings page can connect once per
 * tab; closing the tab abandons the channel and the worker's port set
 * eventually GCs the dead port on the next broadcast.
 *
 * NOT exposed: arbitrary fs / model / bridge proxying. The worker's
 * `webUiBridge` registry is an allow-list of capabilities — only the
 * ones a worker-side module has explicitly registered are reachable.
 * That's by design: a generic WebUI-to-bridge proxy would let any
 * settings page read `~/.ssh/id_rsa` via `fs.readFile`.
 */

// -----------------------------------------------------------------------------
// Bridge protocol shapes (a hand-rolled mirror of nexus_web/src/browser/bridge/
// protocol/core.ts — see "Wire envelope mirrors…" above).
// -----------------------------------------------------------------------------

/** A bridge route — `{namespace, capability, action}`. */
export interface BridgeRoute {
  readonly namespace: 'nexus';
  readonly capability: string;
  readonly action: string;
}

/** Request envelope — what a caller passes to `transport.request`. */
export interface BridgeRequest<P = Record<string, unknown>> {
  readonly id: string;
  readonly route: BridgeRoute;
  readonly payload: P;
}

/** Successful response envelope. */
export interface BridgeSuccessResponse<R = unknown> {
  readonly id: string;
  readonly route: BridgeRoute;
  readonly ok: true;
  readonly result: R;
}

/** Error response envelope. */
export interface BridgeErrorResponse {
  readonly id: string;
  readonly route: BridgeRoute;
  readonly ok: false;
  readonly error: {
    readonly code: string;
    readonly message: string;
    readonly data?: unknown;
  };
}

export type BridgeResponse<R = unknown> =
    BridgeSuccessResponse<R>|BridgeErrorResponse;

/** Worker → WebUI push event. */
export interface BridgeEvent<P = Record<string, unknown>> {
  readonly route: BridgeRoute;
  readonly id?: string;
  readonly payload: P;
}

export type BridgeEventListener = (event: BridgeEvent) => void;
export type BridgeEventUnsubscribe = () => void;

// -----------------------------------------------------------------------------
// Wire shape — must match nexus_web/src/browser/bridge/transport/
// messagePortBridgeTransport.ts exactly. The discriminator is `direction`.
// -----------------------------------------------------------------------------

type WireMessage =|{
  claudeBridge: true; direction: 'request';
  id: string;
  route: BridgeRoute;
  payload: Record<string, unknown>;
}
|{claudeBridge: true; direction: 'response'; id: string; route: BridgeRoute;
   ok: true; result: unknown;
}|{claudeBridge: true; direction: 'response'; id: string;
   route: BridgeRoute; ok: false;
   error: {code: string; message: string; data?: unknown};
}|{claudeBridge: true; direction: 'event'; id?: string; route: BridgeRoute;
   payload: Record<string, unknown>;
};

function isWireMessage(data: unknown): data is WireMessage {
  if (!data || typeof data !== 'object') return false;
  const d = data as Record<string, unknown>;
  if (d['claudeBridge'] !== true) return false;
  return d['direction'] === 'request' || d['direction'] === 'response' ||
      d['direction'] === 'event';
}

// -----------------------------------------------------------------------------
// Connection options + handshake
// -----------------------------------------------------------------------------

export interface ConnectNexusWebUiBridgeOptions {
  /**
   * The nexus_web webview's `contentWindow` — where we send the
   * `nexus-webui-bridge-attach` message. The caller is responsible for
   * locating it (`<webview>.contentWindow` in `<webview>`-based UI,
   * or `iframe.contentWindow` if embedded as an iframe).
   */
  webviewWindow: WindowProxy;
  /**
   * Default request timeout in ms. Past this, pending request Promises
   * reject with a timeout error. Defaults to 30000ms — same as the
   * nexus_web direct-bridge default.
   */
  requestTimeoutMs?: number;
}

export interface NexusWebUiBridgeTransport {
  /**
   * Issue a request and await the response. Resolves with either an
   * `ok: true` or `ok: false` envelope; rejects only on transport-level
   * errors (timeout, disposed, port closed).
   */
  request<R = unknown>(request: BridgeRequest): Promise<BridgeResponse<R>>;

  /**
   * Subscribe to worker → WebUI push events. Returns an unsubscribe.
   * Any number of subscribers can coexist; each receives every event.
   */
  subscribeEvents(listener: BridgeEventListener): BridgeEventUnsubscribe;

  /**
   * Tear down the transport. Pending requests reject with "disposed".
   * The local port end is closed; the worker side's port may stay open
   * for a bit until the worker notices on the next broadcast.
   * Idempotent.
   */
  dispose(): void;
}

/**
 * Open a new WebUI ↔ SharedWorker connection. Posts the attach message
 * to the webview, returns immediately — the attach is fire-and-forget
 * (any later request times out if the worker never received the port,
 * surfacing the misconfiguration the same way as any unresponsive
 * peer).
 */
// --- Attach-handshake race fix -----------------------------------------------
//
// The attach (`nexus-webui-bridge-attach` + transferred port) is fire-once:
// the panel posts it to the guest webview's window, and the guest's page-thread
// `setupWebUiBridgeBootstrap` forwards the port to the SharedWorker. But the
// panel (chrome://custom.top-chrome) boots ~3 s BEFORE the Vite-served guest
// installs that listener. A connection created early (the topbar's session-
// state subscription on mount) posts its attach into the void — the guest
// isn't listening yet, the port is dropped, and the SharedWorker never gets it,
// so no `session.currentStateChanged` events ever reach the topbar (dead pill,
// no auto-open). Settings pages dodged this only because the user opens them
// long after the guest is up.
//
// Fix: the guest emits a `nexus-webui-bridge-ready` beacon to the host window
// when its bootstrap listener installs (and on every guest reload). Every live
// transport re-posts its attach when that beacon arrives — re-establishing any
// connection whose initial one-shot attach was dropped. The eager post on
// connect still covers the guest-already-up case (settings opened late).
type ReattachableConnection = {
  hostWindow: WindowProxy;
  reattach: () => void;
};
const liveBridgeConnections = new Set<ReattachableConnection>();
let readyBeaconListenerInstalled = false;

function ensureReadyBeaconListener(): void {
  if (readyBeaconListenerInstalled) return;
  readyBeaconListenerInstalled = true;
  window.addEventListener('message', (event: MessageEvent) => {
    const data = event.data as Record<string, unknown> | null;
    if (!data || data['type'] !== 'nexus-webui-bridge-ready' ||
        data['nexus'] !== true) {
      return;
    }
    // The guest just (re)installed its bootstrap listener. Re-post every
    // live connection's attach so any whose initial post raced ahead of the
    // guest gets re-established. Match on source window so a beacon from one
    // webview only re-attaches connections aimed at that window.
    for (const conn of liveBridgeConnections) {
      if (event.source && event.source !== conn.hostWindow) continue;
      try {
        conn.reattach();
      } catch {
        /* a single re-attach failure must not block the rest */
      }
    }
  });
}

export function connectNexusWebUiBridge(
    options: ConnectNexusWebUiBridgeOptions): NexusWebUiBridgeTransport {
  let channel = new MessageChannel();
  const timeoutMs = options.requestTimeoutMs ?? 30_000;

  const pending = new Map<string, {
    resolve(response: BridgeResponse): void;
    reject(reason: unknown): void;
    timer: ReturnType<typeof setTimeout>|null;
  }>();
  const eventListeners = new Set<BridgeEventListener>();
  let disposed = false;

  const handleMessage = (event: MessageEvent) => {
    const data = event.data;
    if (!isWireMessage(data)) return;

    if (data.direction === 'response') {
      const entry = pending.get(data.id);
      if (!entry) return;
      pending.delete(data.id);
      if (entry.timer) clearTimeout(entry.timer);
      if (data.ok) {
        entry.resolve({
          id: data.id,
          route: data.route,
          ok: true,
          result: data.result,
        });
      } else {
        entry.resolve({
          id: data.id,
          route: data.route,
          ok: false,
          error: data.error,
        });
      }
      return;
    }

    if (data.direction === 'event') {
      const envelope: BridgeEvent = {
        route: data.route,
        id: data.id,
        payload: data.payload,
      };
      for (const listener of eventListeners) {
        try {
          listener(envelope);
        } catch (e) {
          console.error(
              '[nexus_webui_bridge] event listener threw:',
              e instanceof Error ? e.message : e);
        }
      }
      return;
    }

    // direction === 'request' — the worker is asking the WebUI for
    // something. We don't host handlers here; silently ignore.
  };

  // (Re)create the channel, wire the receive handler, and post the attach +
  // transferred port to the guest. Called eagerly on connect AND again on
  // every `nexus-webui-bridge-ready` beacon (see ensureReadyBeaconListener).
  // Each call uses a FRESH MessageChannel because a transferred port is
  // neutered — re-posting the same port would send a dead handle. The prior
  // channel's port1 is closed first so an early dropped attach doesn't leak.
  //
  // We post '*' for `targetOrigin` because the webview's origin can vary by
  // host (chrome://, http://, etc.) — the page-thread bootstrap source-checks
  // `event.source` and `event.origin` on its side, so a stray third-party
  // catching this postMessage gets no useful information (and the transferred
  // port belongs to us anyway).
  const postAttach = (): void => {
    if (disposed) return;
    try {
      channel.port1.removeEventListener('message', handleMessage);
      channel.port1.close();
    } catch {
      /* first call: nothing to tear down */
    }
    channel = new MessageChannel();
    channel.port1.addEventListener('message', handleMessage);
    channel.port1.start();
    try {
      options.webviewWindow.postMessage(
          {type: 'nexus-webui-bridge-attach', nexus: true},
          '*',
          [channel.port2],
      );
    } catch (e) {
      console.warn(
          '[nexus_webui_bridge] attach postMessage failed:',
          e instanceof Error ? e.message : e);
    }
  };

  const connection: ReattachableConnection = {
    hostWindow: options.webviewWindow,
    reattach: postAttach,
  };
  ensureReadyBeaconListener();
  liveBridgeConnections.add(connection);
  postAttach();

  return {
    request<R = unknown>(req: BridgeRequest): Promise<BridgeResponse<R>> {
      if (disposed) {
        return Promise.reject(new Error('[nexus_webui_bridge] disposed'));
      }
      return new Promise<BridgeResponse<R>>((resolve, reject) => {
        const timer = setTimeout(() => {
          pending.delete(req.id);
          reject(new Error(`[nexus_webui_bridge] request '${
              req.route.capability}.${req.route.action}' (id=${
              req.id}) timed out after ${timeoutMs}ms`));
        }, timeoutMs);
        pending.set(req.id, {
          resolve: resolve as (response: BridgeResponse) => void,
          reject,
          timer,
        });
        const envelope: WireMessage = {
          claudeBridge: true,
          direction: 'request',
          id: req.id,
          route: req.route,
          payload: req.payload,
        };
        try {
          channel.port1.postMessage(envelope);
        } catch (e) {
          pending.delete(req.id);
          if (timer) clearTimeout(timer);
          reject(e);
        }
      });
    },

    subscribeEvents(listener: BridgeEventListener): BridgeEventUnsubscribe {
      eventListeners.add(listener);
      return () => {
        eventListeners.delete(listener);
      };
    },

    dispose(): void {
      if (disposed) return;
      disposed = true;
      liveBridgeConnections.delete(connection);
      channel.port1.removeEventListener('message', handleMessage);
      for (const [id, entry] of pending) {
        if (entry.timer) clearTimeout(entry.timer);
        entry.reject(new Error(
            `[nexus_webui_bridge] disposed before response (id=${id})`));
      }
      pending.clear();
      eventListeners.clear();
      try {
        channel.port1.close();
      } catch {
        /* already closed */
      }
    },
  };
}

/**
 * Convenience: generate a fresh request id. Callers can also bring
 * their own scheme — what matters is that each id is unique within
 * the transport's lifetime so responses correlate.
 */
export function generateBridgeRequestId(): string {
  const crypto = (globalThis as {crypto?: {randomUUID?: () => string}})['crypto'];
  if (crypto?.randomUUID) return crypto.randomUUID();
  return `webui-${Date.now().toString(36)}-${
      Math.random().toString(36).slice(2, 10)}`;
}
