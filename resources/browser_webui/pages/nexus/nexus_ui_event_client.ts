// Copyright 2025 The Custom Browser Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * Browser-side subscriber for the nexus_web `ui` capability's push
 * events.
 *
 * The SharedWorker fires two events that `<nexus-view>` listens for to
 * drive its layout:
 *
 *   - `ui.dismissBootWelcome` — fired once by `nexusBoot.ts` after the
 *     React shell reaches the interactive moment. The host
 *     (`nexus_view.ts`) flips `bootWelcomeDismissed_` and reveals the
 *     guest webview.
 *
 *   - `ui.mainContentsLayout` — fired by the ACTIVE session's
 *     webview_area reporter in the guest (rAF-coalesced, JSON-deduped)
 *     with `{visible, rect?}`. The host mirrors the rect into
 *     `<main-contents-view>`'s absolute position so the native tab
 *     contents paint exactly over the guest's placeholder. Replaces the
 *     legacy latch-once `ui.showMainContents` reveal; the SharedWorker
 *     replays the last payload to each newly attached WebUI port so a
 *     (re)connecting panel paints current state immediately.
 *
 * Why this client exists: the C++ side used to broker these events via
 * `BroadcastWebUiListenerEventToProfile` — pure pass-throughs that did
 * nothing but re-fire the named WebUI event. The SharedWorker can fan
 * the same payload directly to attached WebUI bridge ports (one hop
 * instead of three). Mirrors `nexus_session_state_client.ts`'s
 * subscribe-only pattern.
 *
 * Lifecycle: subscribe-only. No RPC actions. The bridge transport is
 * attached lazily on the first `on*` call; subsequent subscribers
 * share it via the module-level cache. The boot dismiss event has a
 * ~10 s fallback timer in `nexus_view.ts`, so a missed broadcast
 * before the transport attaches degrades gracefully.
 */

import {
  connectNexusWebUiBridge,
  type NexusWebUiBridgeTransport,
} from './nexus_webui_bridge.js';

// -----------------------------------------------------------------------------
// Public surface
// -----------------------------------------------------------------------------

export type UiEventHandler = () => void;

/**
 * Wire shape of the `ui.mainContentsLayout` payload — a hand-rolled
 * mirror of nexus_web's `MainContentsLayoutPayload` (protocol/ui.ts),
 * re-declared here because the WebUI bundle is a separate build (same
 * rationale as the envelope mirror in `nexus_webui_bridge.ts`). Coords
 * are guest-viewport CSS px, pre-rounded; `visible:false` may omit
 * `rect`.
 */
export interface MainContentsLayoutPayload {
  visible: boolean;
  rect?: {left: number; top: number; width: number; height: number};
}

export type MainContentsLayoutHandler =
    (payload: MainContentsLayoutPayload) => void;

// -----------------------------------------------------------------------------
// Transport cache — same shape as nexus_session_state_client.
// -----------------------------------------------------------------------------

type WebviewElement = HTMLElement&{contentWindow?: WindowProxy|null};

let cachedWebview: WebviewElement|null = null;
let cachedTransport: NexusWebUiBridgeTransport|null = null;
let readyPromise: Promise<NexusWebUiBridgeTransport>|null = null;
const dismissHandlers = new Set<UiEventHandler>();
const mainContentsLayoutHandlers = new Set<MainContentsLayoutHandler>();
let subscribedToBridge = false;

const WEBVIEW_READY_TIMEOUT_MS = 30_000;
const WEBVIEW_POLL_INTERVAL_MS = 200;
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
      `nexus-ui-event: webview not ready after ${WEBVIEW_READY_TIMEOUT_MS}ms`);
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
          if (event.route.capability !== 'ui') return;
          if (event.route.action === 'dismissBootWelcome') {
            for (const handler of dismissHandlers) {
              try {
                handler();
              } catch (e) {
                console.error(
                    '[nexus-ui-event] dismissBootWelcome handler threw:', e);
              }
            }
          } else if (event.route.action === 'mainContentsLayout') {
            const payload = event.payload as unknown as
                MainContentsLayoutPayload | undefined;
            if (!payload || typeof payload.visible !== 'boolean') return;
            for (const handler of mainContentsLayoutHandlers) {
              try {
                handler(payload);
              } catch (e) {
                console.error(
                    '[nexus-ui-event] mainContentsLayout handler threw:', e);
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

export function invalidateUiEventWebviewCache(): void {
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
 * Subscribe to `nexus.ui.dismissBootWelcome` push events. Returns an
 * unsubscribe function. Fired by the guest's boot path on the
 * interactive moment; the `nexus_view.ts` host flips
 * `bootWelcomeDismissed_` and reveals the guest webview.
 */
export function onDismissBootWelcome(handler: UiEventHandler): () => void {
  dismissHandlers.add(handler);
  void getTransportAsync().catch(() => {
    /* connection failures surface as missed events; the host has a
     * ~10 s fallback timer in `nexus_view.ts` that copes with this. */
  });
  return () => {
    dismissHandlers.delete(handler);
  };
}

/**
 * Subscribe to `nexus.ui.mainContentsLayout` push events. Returns an
 * unsubscribe function. Fired by the active session's webview_area
 * reporter in the guest; the `nexus_view.ts` host mirrors the payload
 * into `<main-contents-view>`'s absolute position + visibility. The
 * SharedWorker replays the last payload on port attach, so a late
 * subscriber converges without waiting for the next guest relayout.
 */
export function onMainContentsLayout(
    handler: MainContentsLayoutHandler): () => void {
  mainContentsLayoutHandlers.add(handler);
  void getTransportAsync().catch(() => {
    /* connection failures surface as missed events; the worker's
     * replay-on-attach covers reconnects. */
  });
  return () => {
    mainContentsLayoutHandlers.delete(handler);
  };
}
