// Copyright 2024 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Nexus host-layer boot script.
//
// Responsibilities:
//   - Bridge bootstrap: listen for browser ports, forward to guest
//   - Guest webview: configure, load, handshake (nexus-init / nexus-init-ack)
//   - Panel bypass: lock upstream GLIC panels so Nexus guest stays visible
//   - Hosted tool request dispatch: route glic.executeHostedTool bridge
//     requests to the browser-actor-tools executor
//
// Complex hosted-actor task lifecycle lives in hostedActorTaskManager.ts.
// Debug tracing lives in nexusTrace.ts.

import {loadTimeData} from 'chrome://resources/js/load_time_data.js';

import {NexusBridgeBootstrap} from '../nexus_bridge_bootstrap.mojom-webui.js';
import {
  executeHostedTool,
  getCompactableToolNames,
  getHostedToolDefinitions,
  type HostedToolDefinition,
} from './browser-actor-tools/index.js';
import {
  createHostedToolHost,
  getHostedActorContextKey,
  getHostedActorTaskState,
  normalizeHostedToolExecutionContext,
  stopAllHostedActorTasks,
  stopHostedActorTaskForSession,
} from './hostedActorTaskManager.js';
import {
  isNexusMode,
  NEXUS_DEBUG,
  nexusTrace,
  nexusTraceLine,
} from './nexusTrace.js';

// =========================================================================
//  Types
// =========================================================================

interface GuestWebviewElement extends HTMLElement {
  contentWindow: WindowProxy | null;
}

type BrowserHostManifest = {
  hostKind?: string;
  capabilities?: string[];
  toolNames?: string[];
  slashCommands?: string[];
  hostedTools?: HostedToolDefinition[];
  compactableToolNames?: string[];
};

type NexusBridgeBootstrapMessage = {
  type: 'nexus-bridge-bootstrap';
  nexus: true;
  manifest?: BrowserHostManifest;
};

type ClaudeBridgeRoute = {
  namespace?: unknown;
  capability?: unknown;
  action?: unknown;
};

type ClaudeBridgeRequestMessage = {
  claudeBridge: true;
  direction: 'request';
  id: string;
  route: ClaudeBridgeRoute;
  payload: unknown;
};

type ClaudeBridgeResponseMessage = {
  claudeBridge: true;
  direction: 'response';
  id: string;
  route: ClaudeBridgeRoute;
  ok: boolean;
  result?: unknown;
  error?: {
    code: string;
    message: string;
    data?: unknown;
  };
};

// =========================================================================
//  Module state
// =========================================================================

let guestWebview: GuestWebviewElement | null = null;
let guestOrigin: string | null = null;
let guestReadyForBridge = false;
let pendingBridgeBootstrapMessage: NexusBridgeBootstrapMessage | null = null;
let pendingBridgePorts: MessagePort[] | null = null;
let bridgeBootstrapRemote: any = null;
let appRouterInstance: any = undefined;
let nexusInitPingId: number | null = null;
let nexusAckHandler: ((event: MessageEvent<unknown>) => void) | null = null;
let profileReadyFallbackTimerId: number | null = null;
const configuredGuestWebviews = new WeakSet<EventTarget>();

// =========================================================================
//  Constants
// =========================================================================

const PROFILE_READY_STATE_READY = 2;
const PROFILE_READY_FALLBACK_DELAY_MS = 1200;
const PREPARE_FOR_CLIENT_RESULT_SUCCESS = 0;
const PREPARE_FOR_CLIENT_TIMEOUT_MS = 1500;
const PREPARE_FOR_CLIENT_TIMEOUT_SENTINEL = Symbol('prepareForClientTimeout');
const NEXUS_HOSTED_TOOLS: HostedToolDefinition[] = getHostedToolDefinitions();

// =========================================================================
//  Bridge bootstrap
// =========================================================================

function getBridgeBootstrapRemote(): any {
  if (!bridgeBootstrapRemote) {
    bridgeBootstrapRemote = NexusBridgeBootstrap.getRemote();
  }
  return bridgeBootstrapRemote;
}

function parseBootstrapMessage(data: unknown): NexusBridgeBootstrapMessage|null {
  if (typeof data === 'string') {
    try {
      return parseBootstrapMessage(JSON.parse(data));
    } catch {
      return null;
    }
  }
  if (!data || typeof data !== 'object') {
    return null;
  }
  const typedData = data as Record<string, unknown>;
  if (typedData['type'] === 'nexus-bridge-bootstrap' &&
      typedData['nexus'] === true) {
    return typedData as NexusBridgeBootstrapMessage;
  }
  return null;
}

function createHostedToolManifest(
    manifest: BrowserHostManifest | undefined): BrowserHostManifest {
  return {
    ...manifest,
    toolNames: NEXUS_HOSTED_TOOLS.map(tool => tool.name),
    hostedTools: NEXUS_HOSTED_TOOLS,
    compactableToolNames: getCompactableToolNames(),
  };
}

function maybeForwardBridgePortsToGuest(): void {
  if (!guestReadyForBridge || !pendingBridgePorts || pendingBridgePorts.length < 2 ||
      !pendingBridgeBootstrapMessage || !guestWebview?.contentWindow ||
      !guestOrigin) {
    if (NEXUS_DEBUG) {
      nexusTrace('bridge bootstrap waiting', {
        guestReadyForBridge,
        hasPendingPorts: !!pendingBridgePorts,
        pendingPortCount: pendingBridgePorts?.length ?? 0,
        hasPendingBootstrapMessage: !!pendingBridgeBootstrapMessage,
        hasGuestWindow: !!guestWebview?.contentWindow,
        guestOrigin,
      });
    }
    return;
  }

  const message = pendingBridgeBootstrapMessage;
  pendingBridgeBootstrapMessage = null;
  const ports = pendingBridgePorts;
  pendingBridgePorts = null;
  const forwardedMessage: NexusBridgeBootstrapMessage = {
    ...message,
    manifest: createHostedToolManifest(message.manifest),
  };
  if (NEXUS_DEBUG) {
    nexusTrace('forwarding bridge bootstrap', {
      capabilities: forwardedMessage.manifest?.capabilities ?? [],
      toolCount: forwardedMessage.manifest?.hostedTools?.length ?? 0,
      toolNames:
          forwardedMessage.manifest?.hostedTools?.map(tool => tool.name) ?? [],
    });
  }
  guestWebview.contentWindow.postMessage(
      forwardedMessage, guestOrigin, ports);
}

function installBridgeBootstrapListener(): void {
  window.addEventListener('message', (event: MessageEvent<unknown>) => {
    const message = parseBootstrapMessage(event.data);
    if (!message) {
      return;
    }
    if (!event.ports || event.ports.length < 2) {
      if (NEXUS_DEBUG) nexusTrace('ignored bootstrap without required ports');
      return;
    }
    if (NEXUS_DEBUG) {
      nexusTrace('received bootstrap from browser', {
        origin: event.origin,
        portCount: event.ports.length,
        capabilities: message.manifest?.capabilities ?? [],
        toolNames: message.manifest?.toolNames ?? [],
      });
    }
    pendingBridgeBootstrapMessage = message;
    pendingBridgePorts = Array.from(event.ports);
    maybeForwardBridgePortsToGuest();
  });
}

// =========================================================================
//  Hosted tool request dispatch
// =========================================================================

function isClaudeBridgeRequestMessage(data: unknown):
    data is ClaudeBridgeRequestMessage {
  if (!data || typeof data !== 'object') {
    return false;
  }
  const typedData = data as Record<string, unknown>;
  if (typedData['claudeBridge'] !== true ||
      typedData['direction'] !== 'request' || typeof typedData['id'] !== 'string') {
    return false;
  }
  const route = typedData['route'];
  return !!route && typeof route === 'object';
}

function isHostedToolRoute(route: ClaudeBridgeRoute): boolean {
  return route.namespace === 'nexus' && route.capability === 'glic' &&
      route.action === 'executeHostedTool';
}

function isStopSessionActorTaskRoute(route: ClaudeBridgeRoute): boolean {
  return route.namespace === 'nexus' && route.capability === 'glic' &&
      route.action === 'stopSessionActorTask';
}

function postClaudeBridgeResponse(
    target: WindowProxy, origin: string,
    response: ClaudeBridgeResponseMessage): void {
  target.postMessage(response, origin);
}

function getActiveApiHost(): any | null {
  const router = appRouterInstance;
  const glicController = router?.glicController;
  const webviewController = glicController?.webview;
  return webviewController?.host ?? null;
}

// Per-contextKey dispatch chains. Two tool calls that share a contextKey must
// run sequentially, not interleaved: they share a single hosted-actor taskId
// on the browser side, and overlapping PerformActions/StopActorTask traffic
// for the same task can race the upstream GlicActorTaskManager's weak_ptr
// observation continuations and drop the Mojo response callback
// (ProxyToResponder DCHECK in glic.mojom.cc). Calls with different context
// keys remain fully parallel.
const hostedToolDispatchChains = new Map<string, Promise<unknown>>();

function enqueueForContext<T>(
    contextKey: string, work: () => Promise<T>): Promise<T> {
  const previous =
      hostedToolDispatchChains.get(contextKey) ?? Promise.resolve();
  const next = previous.catch(() => undefined).then(work);
  hostedToolDispatchChains.set(contextKey, next);
  // Clear the chain slot once this task settles, but only if no newer task
  // has replaced it — preventing unbounded map growth without breaking
  // ordering for later enqueues.
  void next.finally(() => {
    if (hostedToolDispatchChains.get(contextKey) === next) {
      hostedToolDispatchChains.delete(contextKey);
    }
  });
  return next;
}

function installHostedToolRequestListener(): void {
  window.addEventListener('message', (event: MessageEvent<unknown>) => {
    if (event.origin !== guestOrigin || event.source !== guestWebview?.contentWindow) {
      return;
    }
    if (!isClaudeBridgeRequestMessage(event.data)) {
      return;
    }
    const route = event.data.route;
    if (isHostedToolRoute(route)) {
      handleHostedToolRequest(event.data, event.source as WindowProxy, event.origin);
      return;
    }
    if (isStopSessionActorTaskRoute(route)) {
      handleStopSessionActorTaskRequest(
          event.data, event.source as WindowProxy, event.origin);
      return;
    }
  });
}

function handleHostedToolRequest(
    request: ClaudeBridgeRequestMessage, target: WindowProxy,
    origin: string): void {
  const apiHost = getActiveApiHost();
  if (NEXUS_DEBUG) {
    nexusTrace('received hosted tool request', {
      origin,
      route: request.route,
      hasApiHost: !!apiHost,
    });
  }
  if (!apiHost) {
    postClaudeBridgeResponse(target, origin, {
      claudeBridge: true,
      direction: 'response',
      id: request.id,
      route: request.route,
      ok: false,
      error: {
        code: 'bridge_handler_error',
        message: 'GlicApiHost is not ready',
      },
    });
    return;
  }

  const payload =
      request.payload && typeof request.payload === 'object' ?
      request.payload as Record<string, unknown> :
      {};
  const toolName = typeof payload['name'] === 'string' ? payload['name'] : '';
  const executionContext =
      normalizeHostedToolExecutionContext(payload['executionContext']);
  const args =
      payload['args'] && typeof payload['args'] === 'object' &&
          !Array.isArray(payload['args']) ?
      payload['args'] as Record<string, unknown> :
      {};
  const contextKey = getHostedActorContextKey(executionContext);

  void enqueueForContext(contextKey, async () => {
    const state = getHostedActorTaskState(contextKey);
    const host = createHostedToolHost(apiHost, executionContext);
    if (NEXUS_DEBUG) {
      nexusTrace('dispatching hosted tool', {
        toolName, contextKey, executionContext,
      });
    }
    try {
      const result = await executeHostedTool(
          host, {name: toolName, args}, state.registry);
      if (NEXUS_DEBUG) nexusTrace('hosted tool succeeded', {toolName});
      postClaudeBridgeResponse(target, origin, {
        claudeBridge: true,
        direction: 'response',
        id: request.id,
        route: request.route,
        ok: true,
        result,
      });
    } catch (error: unknown) {
      if (NEXUS_DEBUG) {
        const errObj = (error && typeof error === 'object') ?
            error as {reasonType?: unknown; reason?: unknown} :
            {};
        const {reasonType, reason} = errObj;
        nexusTrace('hosted tool failed', {
          toolName, reasonType, reason,
          message: error instanceof Error ? error.message : String(error),
        });
      }
      postClaudeBridgeResponse(target, origin, {
        claudeBridge: true,
        direction: 'response',
        id: request.id,
        route: request.route,
        ok: false,
        error: {
          code: 'bridge_handler_error',
          message:
              error instanceof Error ? error.message : 'Hosted tool execution failed',
        },
      });
    }
  });
}

// Stop the hosted actor task tracked for the requesting session, if any.
// Enqueued on the same per-contextKey chain as executeHostedTool so we never
// send glicBrowserStopActorTask while a PerformActions response callback is
// still pending (see the ProxyToResponder DCHECK notes in hostedActorTaskManager).
function handleStopSessionActorTaskRequest(
    request: ClaudeBridgeRequestMessage, target: WindowProxy,
    origin: string): void {
  const payload =
      request.payload && typeof request.payload === 'object' ?
      request.payload as Record<string, unknown> :
      {};
  const executionContext =
      normalizeHostedToolExecutionContext(payload['executionContext']);
  const contextKey = getHostedActorContextKey(executionContext);
  const reason =
      typeof payload['reason'] === 'string' && payload['reason'].trim() ?
      payload['reason'].trim() :
      'stop_hook';

  if (NEXUS_DEBUG) {
    nexusTrace('received stopSessionActorTask request', {
      origin, contextKey, reason, executionContext,
    });
  }

  void enqueueForContext(contextKey, async () => {
    try {
      const result = await stopHostedActorTaskForSession(executionContext, reason);
      if (NEXUS_DEBUG) {
        nexusTrace('stopSessionActorTask done', {contextKey, ...result});
      }
      postClaudeBridgeResponse(target, origin, {
        claudeBridge: true,
        direction: 'response',
        id: request.id,
        route: request.route,
        ok: true,
        result: {ok: true, stopped: result.stopped},
      });
    } catch (error: unknown) {
      if (NEXUS_DEBUG) {
        nexusTrace('stopSessionActorTask failed', {
          contextKey,
          message: error instanceof Error ? error.message : String(error),
        });
      }
      postClaudeBridgeResponse(target, origin, {
        claudeBridge: true,
        direction: 'response',
        id: request.id,
        route: request.route,
        ok: false,
        error: {
          code: 'bridge_handler_error',
          message:
              error instanceof Error ? error.message : 'stopSessionActorTask failed',
        },
      });
    }
  });
}

// =========================================================================
//  Guest webview management
// =========================================================================

function buildNexusGuestUrl(): URL {
  const resolvedGuestUrl = new URL(loadTimeData.getString('glicGuestURL'));
  resolvedGuestUrl.searchParams.set('nexus', '1');
  return resolvedGuestUrl;
}

function findGuestWebview(): HTMLElement | null {
  return document.querySelector<HTMLElement>(
      'webview#guestFrame, webview#nexusGuestFrame, webview');
}

function waitForGuestWebview(): Promise<HTMLElement> {
  const existing = findGuestWebview();
  if (existing) {
    return Promise.resolve(existing);
  }
  return new Promise<HTMLElement>((resolve) => {
    const observer = new MutationObserver(() => {
      const webview = findGuestWebview();
      if (!webview) {
        return;
      }
      observer.disconnect();
      resolve(webview);
    });
    observer.observe(document.documentElement, {
      childList: true,
      subtree: true,
    });
  });
}

function clearPendingGuestHandshake(): void {
  if (nexusInitPingId !== null) {
    clearInterval(nexusInitPingId);
    nexusInitPingId = null;
  }
  if (nexusAckHandler) {
    window.removeEventListener('message', nexusAckHandler);
    nexusAckHandler = null;
  }
}

function installNexusGuestListeners(webview: any): void {
  if (configuredGuestWebviews.has(webview)) {
    return;
  }
  configuredGuestWebviews.add(webview);

  webview.addEventListener('contentload', () => {
    if (NEXUS_DEBUG) {
      nexusTrace('guest webview contentload', {
        src: webview.src,
        currentUrl: guestOrigin,
      });
    }
    guestReadyForBridge = false;
    pendingBridgeBootstrapMessage = null;
    pendingBridgePorts = null;
    clearPendingGuestHandshake();

    nexusInitPingId = window.setInterval(() => {
      try {
        if (NEXUS_DEBUG) {
          nexusTrace('sending nexus-init ping', {guestOrigin});
        }
        webview.contentWindow.postMessage(
            {type: 'nexus-init', nexus: true}, guestOrigin!);
      } catch (_e) {
        // Guest not ready yet.
      }
    }, 100);

    nexusAckHandler = (event: MessageEvent<unknown>) => {
      const data = event.data as Record<string, unknown> | null;
      if (data?.['type'] !== 'nexus-init-ack') {
        return;
      }
      if (NEXUS_DEBUG) {
        nexusTrace('received nexus-init-ack', {origin: event.origin});
      }
      clearPendingGuestHandshake();
      guestReadyForBridge = true;
      if (NEXUS_DEBUG) nexusTrace('calling bridge bootstrap guestReady()');
      void getBridgeBootstrapRemote().guestReady();
      maybeForwardBridgePortsToGuest();
    };
    window.addEventListener('message', nexusAckHandler);
  });
}

function configureGuestWebview(webview: any): void {
  clearProfileReadyFallback();
  const resolvedGuestUrl = buildNexusGuestUrl();
  guestWebview = webview as GuestWebviewElement;
  guestOrigin = resolvedGuestUrl.origin;
  if (NEXUS_DEBUG) {
    nexusTrace('configuring guest webview', {
      currentSrc: webview.src,
      targetUrl: resolvedGuestUrl.toString(),
      guestOrigin,
    });
  }

  webview.style.width = '100%';
  webview.style.height = '100%';
  webview.style.border = 'none';
  if (webview.getAttribute('partition') !== 'persist:glicpart') {
    webview.setAttribute('partition', 'persist:glicpart');
  }

  installNexusGuestListeners(webview);
  if (NEXUS_DEBUG) {
    webview.addEventListener('loadcommit', (event: any) => {
      nexusTraceLine(
          `guest webview loadcommit url=${String(event?.url ?? '')} topLevel=${
              String(event?.isTopLevel ?? '')}`);
    });
    webview.addEventListener('loadstop', () => {
      nexusTraceLine('guest webview loadstop');
    });
    webview.addEventListener('loadabort', (event: any) => {
      nexusTraceLine(
          `guest webview loadabort url=${String(event?.url ?? '')} reason=${
              String(event?.reason ?? '')} topLevel=${
              String(event?.isTopLevel ?? '')}`);
    });
    webview.addEventListener('exit', (event: any) => {
      nexusTraceLine(
          `guest webview exit processId=${String(event?.processId ?? '')} reason=${
              String(event?.reason ?? '')}`);
    });
    webview.addEventListener('consolemessage', (event: any) => {
      const level = String(event?.level ?? 'log').toLowerCase();
      const line = String(event?.line ?? '');
      const sourceId = String(event?.sourceId ?? '');
      const message = String(event?.message ?? '');
      const formatted =
          `[NexusTrace][GuestConsole] level=${level} line=${line} source=${sourceId} message=${message}`;
      if (level === 'error' || level === 'warning') {
        console.warn(formatted);
      } else {
        console.info(formatted);
      }
    });
  }

  const targetUrl = resolvedGuestUrl.toString();
  if (webview.src !== targetUrl) {
    if (NEXUS_DEBUG) {
      nexusTrace('updating guest webview src', {
        from: webview.src,
        to: targetUrl,
      });
    }
    webview.src = targetUrl;
  }
}

// =========================================================================
//  Panel bypass
// =========================================================================

function lockPanel(id: string, forceHidden: boolean) {
  const el = document.getElementById(id);
  if (!el) return;
  el.hidden = forceHidden;
  Object.defineProperty(el, 'hidden', {
    get() { return forceHidden; },
    set() { /* blocked by Nexus */ },
    configurable: true,
  });
}

function lockLocalPanels() {
  const section = document.getElementById('localPanels');
  if (!section) return;
  section.classList.add('hidden');
  const origToggle = section.classList.toggle.bind(section.classList);
  section.classList.toggle = function(className: string, force?: boolean) {
    if (className === 'hidden') return true;
    return origToggle(className, force);
  } as DOMTokenList['toggle'];
}

// =========================================================================
//  Profile-ready and prepareForClient fallbacks
// =========================================================================

function clearProfileReadyFallback(): void {
  if (profileReadyFallbackTimerId !== null) {
    window.clearTimeout(profileReadyFallbackTimerId);
    profileReadyFallbackTimerId = null;
  }
}

function scheduleProfileReadyFallback(router: any): void {
  clearProfileReadyFallback();
  profileReadyFallbackTimerId = window.setTimeout(() => {
    profileReadyFallbackTimerId = null;
    const glicController = router?.glicController;
    if (!glicController) return;
    if (findGuestWebview()) return;
    if (glicController.profileReadyState !== undefined) return;
    if (typeof glicController.setProfileReadyState !== 'function') return;
    if (NEXUS_DEBUG) nexusTrace('forcing profileReadyState fallback to ready');
    glicController.setProfileReadyState(PROFILE_READY_STATE_READY);
  }, PROFILE_READY_FALLBACK_DELAY_MS);
}

function wrapPrepareForClient(handler: any): void {
  if (!handler || typeof handler.prepareForClient !== 'function' ||
      handler.__nexusPrepareForClientWrapped) {
    return;
  }
  const originalPrepareForClient = handler.prepareForClient.bind(handler);
  Object.defineProperty(handler, '__nexusPrepareForClientWrapped', {
    value: true,
    configurable: false,
    enumerable: false,
    writable: false,
  });
  handler.prepareForClient = async () => {
    if (NEXUS_DEBUG) nexusTrace('prepareForClient invoked');
    // Nexus has its own provider auth — Google's profile handler can return
    // kRequiresSignIn (2) or kErrorResyncingCookies (1) which would send
    // Glic's state machine to kSignIn/kError and skip webview creation. We
    // let the original run for its side-effects (cookie sync attempts, etc.)
    // but always surface kSuccess so Glic proceeds to construct the guest
    // webview. The race+timeout keeps us from hanging if the original never
    // resolves.
    let originalResult: unknown;
    try {
      originalResult = await Promise.race([
        originalPrepareForClient(),
        new Promise<symbol>((resolve) => {
          window.setTimeout(
              () => resolve(PREPARE_FOR_CLIENT_TIMEOUT_SENTINEL),
              PREPARE_FOR_CLIENT_TIMEOUT_MS);
        }),
      ]);
    } catch (e) {
      if (NEXUS_DEBUG) nexusTrace('prepareForClient threw; forcing success', {error: String(e)});
      originalResult = undefined;
    }
    if (NEXUS_DEBUG) {
      nexusTrace('prepareForClient forcing success', {originalResult});
    }
    return {result: PREPARE_FOR_CLIENT_RESULT_SUCCESS};
  };
}

function installPrepareForClientFallback(router: any): void {
  const browserProxy = router?.browserProxy;
  if (!browserProxy) return;
  wrapPrepareForClient(browserProxy.pageHandler);
  wrapPrepareForClient(browserProxy.glicPreloadHandler);
}

// =========================================================================
//  Force Nexus bypass
// =========================================================================

function tryForceNexusBypass(router: any) {
  if (!router) return;

  if (NEXUS_DEBUG) {
    nexusTrace('tryForceNexusBypass', {
      hasBrowserProxy: !!router.browserProxy,
      hasGlicController: !!router.glicController,
    });
  }
  installPrepareForClientFallback(router);

  if (router.browserProxy && router.browserProxy.pageHandler) {
    if (NEXUS_DEBUG) nexusTrace('forcing page handler ready/resize');
    router.browserProxy.pageHandler.webUiStateChanged(8 /* kReady */);
    router.browserProxy.pageHandler.resizeWidget(
        {width: 400, height: 800}, {microseconds: BigInt(100000)});

    let n = 0;
    const guard = setInterval(() => {
      router.browserProxy.pageHandler.resizeWidget(
          {width: 400, height: 800}, {microseconds: BigInt(100000)});
      if (++n >= 6) clearInterval(guard);
    }, 5000);
  }

  lockPanel('guestPanel', false);
  lockPanel('errorPanel', true);
  lockPanel('loadingPanel', true);
  lockPanel('offlinePanel', true);
  lockPanel('unavailablePanel', true);
  lockPanel('signInPanel', true);
  lockLocalPanels();

  const container = document.getElementById('webviewContainer');
  if (container) {
    const webview = container.querySelector('webview') ?? findGuestWebview();
    if (webview) {
      configureGuestWebview(webview);
    } else {
      void waitForGuestWebview().then((resolvedWebview) => {
        configureGuestWebview(resolvedWebview);
      });
    }
  }
}

// =========================================================================
//  Boot
// =========================================================================

if (isNexusMode) {
  if (NEXUS_DEBUG) {
    nexusTrace('booting nexus host layer', {
      href: location.href,
      guestUrl: loadTimeData.getString('glicGuestURL'),
    });
  }
  // Best-effort cleanup on panel teardown. The browser will detect the Mojo
  // pipe disconnect and close its WebClientHandler receiver regardless, so
  // these sends are not required for correctness — they just shorten the
  // window in which the browser holds the old task. `beforeunload` was
  // removed: it can surface a confirmation prompt in some embeddings and
  // duplicates the `pagehide` path.
  window.addEventListener('pagehide', () => {
    stopAllHostedActorTasks('pagehide');
  });
  installBridgeBootstrapListener();
  installHostedToolRequestListener();

  loadTimeData.overrideValues({
    'shouldShowFre': false,
    'reloadAfterNavigation': false,
  });

  Object.defineProperty(window, 'appRouter', {
    get: () => appRouterInstance,
    set: (val: any) => {
      appRouterInstance = val;
      if (NEXUS_DEBUG) {
        nexusTrace('window.appRouter assigned', {
          hasGlicController: !!val?.glicController,
          hasBrowserProxy: !!val?.browserProxy,
        });
      }
      scheduleProfileReadyFallback(val);
      tryForceNexusBypass(val);
    },
    configurable: true,
  });
}
