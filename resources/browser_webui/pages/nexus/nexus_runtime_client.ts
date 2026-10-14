// Copyright 2025 VellocLabs. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * The panel host as a client of the UI <-> Runtime protocol: a handshake, one
 * subscription (`shell/<panelId>`) and a dispatch; it issues no requests.
 * `protocol/` is a generated copy of the guest's codec. The panel hands a
 * `MessagePort` to the guest page, which forwards it to the SharedWorker
 * (`bridge.attachRuntimePort`).
 */

// Straight from the mirrored files, not a barrel: the mirror carries the codec
// and nothing else, so there is no index to re-export it.
import {readFrame} from './protocol/runtimeProtocol/codec.js';
import {
  RUNTIME_PROTOCOL_VERSION,
  TOPICS_SUBSCRIBE_ROUTE,
  type EvtFrame,
  type Frame,
  type WelcomeFrame,
} from './protocol/runtimeProtocol/frames.js';

/** One event on the subscribed topic. `type` says which of the four it is. */
export interface ShellEvent {
  type: string|undefined;
  payload: unknown;
  form: 'snapshot'|'delta'|'deltas';
}

export type ShellEventHandler = (event: ShellEvent) => void;

export interface RuntimeClientOptions {
  /** This panel's id — what binds the connection to `shell/<panelId>`. */
  panelId: string;
  /** Hands the transferred port to the guest; see `nexus_webui_bridge.ts`. */
  attach: (port: MessagePort, panelId: string) => void;
  onEvent: ShellEventHandler;
  /** Called once the runtime answers `welcome`. */
  onReady?: (welcome: WelcomeFrame) => void;
}

export interface RuntimeClientHandle {
  /** Stop listening and drop the port. Idempotent. */
  dispose(): void;
}

let nextRequestId = 1;

/**
 * Open one connection and subscribe to this panel's shell topic.
 *
 * Failure is silent by design: every signal on this topic has a fallback in
 * `nexus_view.ts` (the dismiss timer, the last known rect), because a panel
 * that cannot reach the runtime must still show a usable window — including
 * against a guest whose protocol version this build predates.
 */
export function connectPanelRuntime(options: RuntimeClientOptions):
    RuntimeClientHandle {
  const channel = new MessageChannel();
  const port = channel.port1;
  let disposed = false;
  let subscribed = false;

  // On a MessagePort a frame travels as ITSELF (structured clone), and the
  // receiver accepts it iff `readFrame` — the same rule both ends of the guest's
  // transport follow (a value this build does not know reads as its fallback;
  // the guest is web-deployed and may be newer than this binary).
  // `encodeFrame` is for a byte pipe, which this is not.
  const send = (frame: Frame): void => {
    try {
      port.postMessage(frame);
    } catch {
      // The port died with the guest; a reload re-attaches.
    }
  };

  const onMessage = (event: MessageEvent): void => {
    if (disposed) {
      return;
    }
    const frame: Frame | null = readFrame(event.data);
    if (!frame) {
      return;
    }
    if (frame.kind === 'welcome') {
      options.onReady?.(frame);
      if (!subscribed) {
        subscribed = true;
        send({
          kind: 'req',
          id: `panel-${nextRequestId++}`,
          route: TOPICS_SUBSCRIBE_ROUTE,
          payload: {topics: [{topic: `shell/${options.panelId}`}]},
        });
      }
      return;
    }
    if (frame.kind === 'evt') {
      const evt = frame as EvtFrame;
      options.onEvent({type: evt.type, payload: evt.payload, form: evt.form});
      return;
    }
    // `bye` (including a version the runtime does not serve) leaves this page
    // on its fallbacks rather than in a retry loop: a panel is rebuilt with
    // the browser, and the guest that outgrew it will come with a new one.
  };

  port.addEventListener('message', onMessage);
  port.start();

  options.attach(channel.port2, options.panelId);

  send({
    kind: 'hello',
    v: RUNTIME_PROTOCOL_VERSION,
    client: {
      clientId: `panel-host-${options.panelId}`,
      kind: 'panel-host',
      panelId: options.panelId,
    },
    auth: {mode: 'local'},
  });

  return {
    dispose(): void {
      if (disposed) {
        return;
      }
      disposed = true;
      port.removeEventListener('message', onMessage);
      try {
        port.close();
      } catch {
        // Already closed.
      }
    },
  };
}
