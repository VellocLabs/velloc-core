// utils/mouse_capture_util.ts
// Small helper around the native startCapture/CaptureCompleted mojo plumbing.
// Generates unique ids, forwards completion callbacks, and returns a handle
// with a promise that resolves when the capture ends.

import {BrowserApiProxyImpl} from '../browser_api_proxy.js';
import type {Rect as gfxRect}
  from '//resources/mojo/ui/gfx/geometry/mojom/geometry.mojom-webui.js';

export type CaptureCompletedCallback = (id: number) => void;

export interface StartCaptureOptions {
  delayMs?: number;
  onComplete?: CaptureCompletedCallback;
}

export interface CaptureHandle {
  id: number;
  completed: Promise<void>;
  cancel(): void;
}

type PendingCapture = {
  resolve: () => void;
  callback?: CaptureCompletedCallback;
};

let nextCaptureId = 1;
const pendingCaptures = new Map<number, PendingCapture>();
let routerHooked = false;

function ensureRouterHook() {
  if (routerHooked) {
    return;
  }
  const router =
      BrowserApiProxyImpl.getInstance().getNativeViewCallbackRouter();
  router.captureCompleted.addListener(
      (captureId: number) => finalizeCapture(captureId, /*invokeCallback=*/true));
  routerHooked = true;
}

export function startElementCapture(
    element: HTMLElement,
    options: StartCaptureOptions = {}): CaptureHandle {
  if (!(element instanceof HTMLElement)) {
    throw new Error('startElementCapture: element must be an HTMLElement');
  }

  ensureRouterHook();

  const id = nextCaptureId++;
  const bounds = elementToGfxRect(element);
  const delayMs = Math.max(0, Math.floor(options.delayMs ?? 0));

  const completed = new Promise<void>(resolve => {
    pendingCaptures.set(id, {resolve, callback: options.onComplete});
  });

  try {
    BrowserApiProxyImpl.getInstance()
        .getNativeViewHandler()
        .startCapture(bounds, id, delayMs);
  } catch (_err) {
    finalizeCapture(id, /*invokeCallback=*/true);
  }

  return {
    id,
    completed,
    cancel() {
      finalizeCapture(id, /*invokeCallback=*/false);
    },
  };
}

function finalizeCapture(id: number, invokeCallback: boolean) {
  const pending = pendingCaptures.get(id);
  if (!pending) {
    return;
  }
  pendingCaptures.delete(id);

  try {
    if (invokeCallback && pending.callback) {
      pending.callback(id);
    }
  } finally {
    pending.resolve();
  }
}

function elementToGfxRect(el: HTMLElement): gfxRect {
  const r = el.getBoundingClientRect();
  const x = Math.floor(r.left);
  const y = Math.floor(r.top);
  const right = Math.ceil(r.right);
  const bottom = Math.ceil(r.bottom);
  return {x, y, width: right - x, height: bottom - y};
}
