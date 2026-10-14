// Copyright 2025 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

/**
 * The window chrome's feed: `shell/<panelId>` on the runtime connection
 * (layout, boot progress, build info, overlay dismissal). The topic opens with
 * a snapshot, so a late panel paints current geometry at once; subscriptions
 * are per panel, so another window's frames cannot move this one's views.
 */

import {connectPanelRuntime, type RuntimeClientHandle} from
    './nexus_runtime_client.js';
import {panelId} from './nexus_panel_id.js';

export type UiEventHandler = () => void;

/**
 * Where the guest's webview_area placeholder is, in guest-viewport CSS px,
 * pre-rounded. `visible:false` may omit `rect`. Declared here rather than
 * imported from the guest bundle: the two are separate builds, and the payload
 * is additive-and-ignore-if-absent by contract.
 */
export interface MainContentsLayoutPayload {
  visible: boolean;
  rect?: {left: number; top: number; width: number; height: number};
}

export type MainContentsLayoutHandler =
    (payload: MainContentsLayoutPayload) => void;

/** The guest bundle's build time (an ISO string) for the overlay's footer. */
export interface BuildInfoPayload {
  buildTime: string;
}

export type BuildInfoHandler = (payload: BuildInfoPayload) => void;

/**
 * A boot stage the guest reports once the host handshake can no longer see
 * progress. `stage` is a `LoadingStage` string, kept as a bare `string` so a
 * newer guest can add stages without breaking this handler.
 */
export interface BootProgressPayload {
  stage: string;
}

export type BootProgressHandler = (payload: BootProgressPayload) => void;

// -----------------------------------------------------------------------------
// The connection — one per page, opened on the first subscriber.
// -----------------------------------------------------------------------------

type WebviewElement = HTMLElement&{contentWindow?: WindowProxy|null};

const WEBVIEW_READY_TIMEOUT_MS = 30_000;
const WEBVIEW_POLL_INTERVAL_MS = 200;

const dismissHandlers = new Set<UiEventHandler>();
const layoutHandlers = new Set<MainContentsLayoutHandler>();
const buildInfoHandlers = new Set<BuildInfoHandler>();
const bootProgressHandlers = new Set<BootProgressHandler>();

let cachedWebview: WebviewElement|null = null;
let client: RuntimeClientHandle|null = null;
let connecting: Promise<void>|null = null;
/** Set once the runtime answers `welcome` — i.e. the port really landed. */
let connected = false;

// The panel boots seconds before the guest installs its port-forwarding
// listener, so an attach sent too early goes into the void. The guest posts
// this beacon once that listener is up; if we are still unanswered, throw the
// dead channel away and attach again. (The legacy bridge did the same thing
// for the same reason.)
window.addEventListener('message', (event: MessageEvent) => {
  const data = event.data as Record<string, unknown>|null;
  if (data?.['type'] !== 'nexus-webui-bridge-ready' || data['nexus'] !== true) {
    return;
  }
  if (connected || (!client && !connecting)) {
    return;
  }
  client?.dispose();
  client = null;
  ensureConnected();
});

/** The guest `<webview>`, wherever it sits in the shadow trees. */
function resolveNexusWebview(): WebviewElement|null {
  if (cachedWebview && cachedWebview.isConnected) {
    return cachedWebview;
  }
  const seen = new Set<Node>();
  const queue: Array<Document|ShadowRoot> = [document];
  while (queue.length > 0) {
    const root = queue.shift();
    if (!root || seen.has(root)) {
      continue;
    }
    seen.add(root);
    const node = root.querySelector('#nexusPane');
    if (node) {
      cachedWebview = node as WebviewElement;
      return cachedWebview;
    }
    for (const el of Array.from(root.querySelectorAll('*'))) {
      const shadow = (el as Element&{shadowRoot?: ShadowRoot}).shadowRoot;
      if (shadow && !seen.has(shadow)) {
        queue.push(shadow);
      }
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
    await new Promise(
        resolve => window.setTimeout(resolve, WEBVIEW_POLL_INTERVAL_MS));
  }
  throw new Error(
      `nexus-shell: webview not ready after ${WEBVIEW_READY_TIMEOUT_MS}ms`);
}

/** Snapshot payload of `shell/<panelId>` — the panel's whole chrome state. */
interface ShellSnapshot {
  layout?: MainContentsLayoutPayload|null;
  bootProgress?: BootProgressPayload|null;
  buildInfo?: BuildInfoPayload|null;
  dismissed?: boolean;
}

function applySnapshot(snapshot: ShellSnapshot): void {
  if (snapshot.layout) {
    for (const handler of layoutHandlers) {
      handler(snapshot.layout);
    }
  }
  if (snapshot.bootProgress) {
    for (const handler of bootProgressHandlers) {
      handler(snapshot.bootProgress);
    }
  }
  if (snapshot.buildInfo) {
    for (const handler of buildInfoHandlers) {
      handler(snapshot.buildInfo);
    }
  }
  if (snapshot.dismissed) {
    for (const handler of dismissHandlers) {
      handler();
    }
  }
}

function applyDelta(type: string|undefined, payload: unknown): void {
  switch (type) {
    case 'layout':
      for (const handler of layoutHandlers) {
        handler(payload as MainContentsLayoutPayload);
      }
      return;
    case 'bootProgress':
      for (const handler of bootProgressHandlers) {
        handler(payload as BootProgressPayload);
      }
      return;
    case 'buildInfo':
      for (const handler of buildInfoHandlers) {
        handler(payload as BuildInfoPayload);
      }
      return;
    case 'dismissed':
      for (const handler of dismissHandlers) {
        handler();
      }
      return;
    default:
      // A newer guest publishing something this build does not know about.
      return;
  }
}

function ensureConnected(): void {
  if (client || connecting) {
    return;
  }
  connecting = (async () => {
    try {
      const webview = await waitForWebviewReady();
      const guestWindow = webview.contentWindow as WindowProxy;
      client = connectPanelRuntime({
        panelId: panelId(),
        onReady: () => {
          connected = true;
        },
        attach: (port, id) => {
          guestWindow.postMessage(
              {type: 'nexus-runtime-attach', nexus: true, panelId: id}, '*',
              [port]);
        },
        onEvent: event => {
          if (event.form === 'snapshot') {
            applySnapshot((event.payload ?? {}) as ShellSnapshot);
            return;
          }
          applyDelta(event.type, event.payload);
        },
      });
    } catch (e) {
      // Every signal here has a fallback in `nexus_view.ts` (the dismiss
      // timer, the last known rect), so a panel that cannot reach the runtime
      // still shows a usable window.
      console.warn(
          `[nexus-shell] not connected: ${
              e instanceof Error ? e.message : String(e)}`);
    } finally {
      connecting = null;
    }
  })();
}

/**
 * Drop the connection: the guest reloaded, so its ports are gone. Called from
 * the bootstrap on every guest `contentload`. With subscribers present the
 * attach is re-driven at once; the guest's ready beacon then re-attaches
 * again if this one was posted before its listener was up.
 */
export function invalidateShellClient(): void {
  cachedWebview = null;
  connected = false;
  client?.dispose();
  client = null;
  if (dismissHandlers.size > 0 || layoutHandlers.size > 0 ||
      buildInfoHandlers.size > 0 || bootProgressHandlers.size > 0) {
    ensureConnected();
  }
}

function subscribe<T>(set: Set<T>, handler: T): () => void {
  set.add(handler);
  ensureConnected();
  return () => {
    set.delete(handler);
  };
}

export function onDismissBootWelcome(handler: UiEventHandler): () => void {
  return subscribe(dismissHandlers, handler);
}

export function onMainContentsLayout(handler: MainContentsLayoutHandler):
    () => void {
  return subscribe(layoutHandlers, handler);
}

export function onBuildInfo(handler: BuildInfoHandler): () => void {
  return subscribe(buildInfoHandlers, handler);
}

export function onBootProgress(handler: BootProgressHandler): () => void {
  return subscribe(bootProgressHandlers, handler);
}
