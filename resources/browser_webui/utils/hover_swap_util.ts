// Generic "hover to reveal" helper for WebUI. While shown it runs a native
// capture against `enterEl`'s bounds, so leaving the WebUI is still detected;
// falls back to DOM-only tracking if native capture fails.

import {startElementCapture, type CaptureHandle} from './mouse_capture_util.js';
import {BrowserApiProxyImpl} from '../browser_api_proxy.js';

export type HoverSwapHandle = {
  show(): void;
  hide(): void;
  destroy(): void;
  isRevealed(): boolean;
};

export type HoverEnterCallback =
    (revealed: boolean) => void;
export type TransitionedCallback =
    (propertyName: string) => boolean | void;

export interface InstallHoverSwapOptions {
  enterEl: HTMLElement;
  targetEl?: HTMLElement;
  // Attribute toggle host (defaults to inferred shadow host or enterEl)
  // attributeHost?: Element | null;
  hoverEnter: HoverEnterCallback;
  transitioned?: TransitionedCallback;
  hasEntered?: boolean;
  // UX tuning
  leaveDelayMs?: number;       // default: 120 (helps avoid flicker on diagonal exits)
}

export function installHoverSwap(opts: InstallHoverSwapOptions): HoverSwapHandle {
  const {
    enterEl,
    targetEl,
    hoverEnter,
    transitioned,
    leaveDelayMs: leaveDelayMsParam,
  } = opts;

  if (!(enterEl instanceof HTMLElement)) {
    throw new Error('installHoverSwap: enterEl must be an HTMLElement');
  }

  const leaveDelayMs = Math.max(0, Math.floor(leaveDelayMsParam ?? 120));

  let revealed = false;
  let activeCapture: CaptureHandle | null = null;
  let transitionHandler: ((event: TransitionEvent) => void) | null = null;

  if (opts.hasEntered) {
    // Pre-entered state.
    revealed = true;
    ensureNativeCaptureAndTracking();
  } else {
    // Initial state: not revealed.
    applyState(false);
  }

  function applyState(next: boolean) {
    BrowserApiProxyImpl.getInstance()
        .getNativeViewHandler()
        .enableLayoutsSaving(next === false);

    revealed = next;
    hoverEnter(revealed);
    enterEl.removeAttribute('aria-hidden');
  }

  function getTargetElement(): HTMLElement {
    return targetEl ?? enterEl;
  }

  function clearTransitionWatch() {
    if (transitionHandler) {
      getTargetElement().removeEventListener('transitionend', transitionHandler);
      transitionHandler = null;
    }
  }

  function startRevealTracking() {
    clearTransitionWatch();
    const target = getTargetElement();
    if (shouldWaitForTransition( target, Boolean(transitioned))) {
      transitionHandler = (event: TransitionEvent) => {
        if (event.target !== target) {
          return;
        }
        let accepted = true;
        if (transitioned) {
          const result = transitioned(event.propertyName);
          accepted = result !== false;
        } else {
          accepted = isDefaultTransitionProperty(event.propertyName);
        }
        if (accepted) {
          finalizeReveal(event.propertyName, /*transitionNotified=*/true);
        }
      };
      target.addEventListener('transitionend', transitionHandler);
      return;
    }
    finalizeReveal('', /*transitionNotified=*/false);
  }

  function finalizeReveal(propertyName: string, transitionNotified: boolean) {
    if (!revealed) {
      return;
    }
    clearTransitionWatch();
    if (!transitionNotified) {
      if (transitioned) {
        transitioned(propertyName);
      }
    }
    ensureNativeCaptureAndTracking();
  }

  function showNow() {
    if (revealed) {
      return;
    }
    applyState(true);
    startRevealTracking();
  }

  function hideNow() {
    if (!revealed) {
      return;
    }
    clearTransitionWatch();
    stopCapture();
    applyState(false);
  }

  // ----- Native capture + move-out tracking ---------------------------------

  function stopCapture() {
    if (!activeCapture) {
      return;
    }
    activeCapture.cancel();
    activeCapture = null;
  }

  async function ensureNativeCaptureAndTracking() {
    // Wait a frame so enterEl has correct layout before measuring.
    await nextAF();
    if (!revealed) {
      return;
    }
    stopCapture();
    activeCapture = startElementCapture(getTargetElement(), {
      onComplete: (id: number) => onCaptureFinished(id),
      delayMs: leaveDelayMs,
    });
  }

  function onCaptureFinished(id: number) {
    if (!activeCapture || activeCapture.id !== id) {
      return;
    }
    activeCapture = null;
    hideNow();
  }


  function nextAF(): Promise<void> {
    return new Promise(resolve => requestAnimationFrame(() => resolve()));
  }

  function shouldWaitForTransition(el: Element, watchAny: boolean): boolean {
    const style = getComputedStyle(el);
    return style.transitionProperty.split(',').some(prop => {
      const name = prop.trim();
      if (name === '' || name === 'none') {
        return false;
      }
      if (watchAny) {
        return true;
      }
      return isDefaultTransitionProperty(name);
    });
  }

  function isDefaultTransitionProperty(propertyName: string): boolean {
    return propertyName === 'height' || propertyName === 'all' || propertyName === 'width';
  }

  // ----- Trigger wiring ------------------------------------------------------
  // Pointer logic: entering A opens.
  enterEl.addEventListener('pointerenter', showNow);
  // Accessibility: open when A is focused.
  enterEl.addEventListener('focusin', showNow);

  // Programmatic API ----------------------------------------------------------
  return {
    show: showNow,
    hide: hideNow,
    destroy() {
      clearTransitionWatch();
      stopCapture();

      enterEl.removeEventListener('pointerenter', showNow);
      enterEl.removeEventListener('focusin', showNow);
      // Restore initial state.
      applyState(false);
    },
    isRevealed() { return revealed; },
  };
}
