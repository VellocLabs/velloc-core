// Copyright 2025 The Custom Browser Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * Browser-side subscriber for the nexus_web `session` capability's
 * push events.
 *
 * The SharedWorker fires two events that the chromium topbar +
 * quick-settings + session list listen for:
 *
 *   - `session.currentStateChanged` — payload shape mirrors what the
 *     legacy C++-relayed `addWebUiListener('nexusCurrentSessionState', …)`
 *     channel used to deliver. Consumed by `nexus_topbar.ts` (phase
 *     pill, model name) and `nexus_quick_settings.ts` (active model
 *     highlight).
 *
 *   - `session.summaryChanged` — same shape as the legacy
 *     `nexusSessionsSummary` payload. Consumed by `nexus_session_list.ts`.
 *
 * Why this client exists: the C++ side used to broker both events via
 * `BroadcastWebUiListenerEventToProfile`. Those handlers were pure
 * pass-throughs (the SharedWorker already did snapshot/delta/dedup
 * before firing). The WebUI bridge ↔ SharedWorker port can fan the
 * same payload directly — one hop instead of three — so the C++ pass-
 * through was dropped and these subscribers replaced the legacy
 * `addWebUiListener` calls.
 *
 * Lifecycle: subscribe-only. No RPC actions. The bridge transport is
 * opened lazily on the first `on*Changed` call; subsequent panel pages
 * share it via the module-level cache. Disconnect is automatic on
 * page unload (the bridge port closes; the worker reaps the dead
 * port on the next broadcast).
 */

import {
  connectNexusWebUiBridge,
  type NexusWebUiBridgeTransport,
} from './nexus_webui_bridge.js';

// -----------------------------------------------------------------------------
// Public types — match nexus_web payload shapes that the worker fans out.
// Keep these in lock-step with:
//   - nexus_web/src/browser/worker/SharedWorkerSessionManager.ts (firing site)
//   - nexus_topbar.ts / nexus_quick_settings.ts / nexus_session_list.ts
//     (consumers; their existing local types are the contract).
// -----------------------------------------------------------------------------

/** Generic handler signature — the wire payload is opaque at this
 *  layer because the three consumer pages each have their own
 *  stricter narrow type. Each subscriber re-validates at its own
 *  boundary; this client is a transport, not a parser. */
export type CurrentSessionStateHandler = (payload: unknown) => void;
export type SessionsSummaryHandler = (payload: unknown) => void;

// -----------------------------------------------------------------------------
// Transport — same caching pattern as nexus_mcp_client.
// -----------------------------------------------------------------------------

type WebviewElement = HTMLElement&{contentWindow?: WindowProxy|null};

let cachedWebview: WebviewElement|null = null;
let cachedTransport: NexusWebUiBridgeTransport|null = null;
let readyPromise: Promise<NexusWebUiBridgeTransport>|null = null;
const currentStateHandlers = new Set<CurrentSessionStateHandler>();
const summaryHandlers = new Set<SessionsSummaryHandler>();
let subscribedToBridge = false;

const WEBVIEW_READY_TIMEOUT_MS = 30_000;
const WEBVIEW_POLL_INTERVAL_MS = 200;
/** Subscribers don't make requests — events arrive unsolicited — so
 *  this timeout only governs the initial attach handshake. */
const PER_REQUEST_TIMEOUT_MS = 5_000;

function resolveNexusWebview(): WebviewElement|null {
  if (cachedWebview && cachedWebview.isConnected) {
    return cachedWebview;
  }
  const seen = new Set<Node>();
  const queue: Array<Document|ShadowRoot> = [document];
  while (queue.length > 0) {
    const root = queue.shift();
    if (!root || seen.has(root)) continue;
    seen.add(root);
    const node = root.querySelector('#nexusPane');
    if (node) {
      cachedWebview = node as WebviewElement;
      return cachedWebview;
    }
    for (const el of Array.from(root.querySelectorAll('*'))) {
      const sh = (el as Element & {shadowRoot?: ShadowRoot}).shadowRoot;
      if (sh && !seen.has(sh)) queue.push(sh);
    }
  }
  cachedWebview = null;
  return null;
}

async function waitForWebviewReady(): Promise<WebviewElement> {
  const startTime = Date.now();
  while (Date.now() - startTime < WEBVIEW_READY_TIMEOUT_MS) {
    const webview = resolveNexusWebview();
    if (webview?.contentWindow) {
      return webview;
    }
    await new Promise(resolve =>
        window.setTimeout(resolve, WEBVIEW_POLL_INTERVAL_MS));
  }
  throw new Error(
      `nexus-session-state: webview not ready after ${WEBVIEW_READY_TIMEOUT_MS}ms`);
}

async function getTransportAsync(): Promise<NexusWebUiBridgeTransport> {
  if (cachedTransport && cachedWebview && cachedWebview.isConnected) {
    return cachedTransport;
  }
  if (cachedTransport &&
      (!cachedWebview || !cachedWebview.isConnected)) {
    cachedTransport.dispose();
    cachedTransport = null;
    subscribedToBridge = false;
  }
  if (readyPromise) return readyPromise;

  readyPromise = (async () => {
    try {
      const webview = await waitForWebviewReady();
      cachedTransport = connectNexusWebUiBridge({
        webviewWindow: webview.contentWindow as WindowProxy,
        requestTimeoutMs: PER_REQUEST_TIMEOUT_MS,
      });
      if (!subscribedToBridge) {
        cachedTransport.subscribeEvents(event => {
          if (event.route.capability !== 'session') return;
          if (event.route.action === 'currentStateChanged') {
            for (const handler of currentStateHandlers) {
              try {
                handler(event.payload);
              } catch (e) {
                console.error(
                    '[nexus-session-state] currentStateChanged handler threw:', e);
              }
            }
          } else if (event.route.action === 'summaryChanged') {
            for (const handler of summaryHandlers) {
              try {
                handler(event.payload);
              } catch (e) {
                console.error(
                    '[nexus-session-state] summaryChanged handler threw:', e);
              }
            }
          }
        });
        subscribedToBridge = true;
      }
      return cachedTransport;
    } finally {
      readyPromise = null;
    }
  })();
  return readyPromise;
}

export function invalidateSessionStateWebviewCache(): void {
  cachedWebview = null;
  if (cachedTransport) {
    cachedTransport.dispose();
    cachedTransport = null;
    subscribedToBridge = false;
  }
}

// -----------------------------------------------------------------------------
// Public subscribers
// -----------------------------------------------------------------------------

/**
 * Subscribe to `session.currentStateChanged` push events. Returns an
 * unsubscribe function. Fired by the SharedWorker on every
 * `flushCurrentSnapshot` (after phase derivation, snapshot/delta
 * selection, and dedup). Replaces the legacy
 * `addWebUiListener('nexusCurrentSessionState', …)` channel.
 */
export function onCurrentSessionStateChanged(
    handler: CurrentSessionStateHandler): () => void {
  currentStateHandlers.add(handler);
  void getTransportAsync().catch(() => {
    /* connection failures surface as missing events */
  });
  return () => {
    currentStateHandlers.delete(handler);
  };
}

/**
 * Subscribe to `session.summaryChanged` push events. Returns an
 * unsubscribe function. Fired by the SharedWorker on every
 * `flushSummary` (coalesced; sessions-list rows). Replaces the
 * legacy `addWebUiListener('nexusSessionsSummary', …)` channel.
 */
export function onSessionsSummaryChanged(
    handler: SessionsSummaryHandler): () => void {
  summaryHandlers.add(handler);
  void getTransportAsync().catch(() => {
    /* connection failures surface as missing events */
  });
  return () => {
    summaryHandlers.delete(handler);
  };
}
