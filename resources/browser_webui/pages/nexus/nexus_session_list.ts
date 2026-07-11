// Copyright 2025 The Custom Browser Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {onSessionsSummaryChanged} from './nexus_session_state_client.js';
import {
  CrLitElement,
  css,
  html,
  nothing,
  type CSSResultGroup,
} from 'chrome://resources/lit/v3_0/lit.rollup.js';

// Wire format for `nexusSessionsSummary`. Sourced from
// `SharedWorkerSessionManager.buildSummaryPayload` (see nexus_web's
// `bridge/protocol/ui.ts`). Always a full snapshot: every known
// session's running flag and counters. Coalesced 100 ms at source.
export interface NexusSessionsSummaryRow {
  sessionId: string;
  running: boolean;
  tool: number;
  toolFail: number;
  mcp: number;
  mcpFail: number;
}

export interface NexusSessionsSummaryPayload {
  sessions: NexusSessionsSummaryRow[];
}

// `SUMMARY_EVENT` was the addWebUiListener name for the legacy
// C++-relayed channel. Removed — `onSessionsSummaryChanged` from
// `nexus_session_state_client.ts` subscribes via the WebUI bridge
// directly.

export class NexusSessionList extends CrLitElement {
  static get is() {
    return 'nexus-session-list';
  }

  static override get styles(): CSSResultGroup {
    return css`
      :host {
        display: block;
        font-family: var(--cr-default-font-family, system-ui);
        font-size: 12px;
        color: var(--cr-primary-text-color, #202124);
      }
      .row {
        display: grid;
        grid-template-columns: 1fr auto auto auto;
        gap: 8px;
        align-items: center;
        padding: 4px 8px;
        border-radius: 4px;
      }
      .row + .row {
        border-top: 1px solid
            var(--cr-separator-color, rgba(0, 0, 0, 0.08));
      }
      .row--running {
        background: var(--cr-active-row-background, rgba(26, 115, 232, 0.08));
      }
      .id {
        font-family: var(--cr-monospace-font-family, monospace);
        white-space: nowrap;
        overflow: hidden;
        text-overflow: ellipsis;
      }
      .dot {
        width: 8px;
        height: 8px;
        border-radius: 50%;
        background: var(--cr-secondary-text-color, #9aa0a6);
      }
      .dot--running {
        background: #1a73e8;
      }
      code {
        font-family: var(--cr-monospace-font-family, monospace);
      }
      .empty {
        padding: 12px;
        color: var(--cr-secondary-text-color, #5f6368);
      }
    `;
  }

  private rows_: NexusSessionsSummaryRow[] = [];
  // `unsubscribe_` replaces the legacy
  // `addWebUiListener('nexusSessionsSummary', …)` that C++ relayed as
  // a pure pass-through. The SharedWorker now fans the same payload
  // directly through the WebUI bridge — see
  // `nexus_session_state_client.ts` for the subscriber.
  private unsubscribe_: (() => void)|null = null;

  constructor() {
    super();
    // Register early so we don't lose the first broadcast the
    // SharedWorker manager fires on connect. The bridge transport is
    // lazily attached on first subscribe; the worker queues no events
    // pre-attach, so any miss before this line is a no-op (matches
    // the legacy behaviour — `addWebUiListener` also missed pre-attach
    // events).
    this.unsubscribe_ = onSessionsSummaryChanged(
        (payload: unknown) =>
            this.onSummary_(payload as NexusSessionsSummaryPayload));
  }

  override disconnectedCallback(): void {
    if (this.unsubscribe_) {
      this.unsubscribe_();
      this.unsubscribe_ = null;
    }
    super.disconnectedCallback();
  }

  private onSummary_(payload: NexusSessionsSummaryPayload): void {
    if (!payload || !Array.isArray(payload.sessions)) return;
    this.rows_ = payload.sessions;
    this.requestUpdate();
  }

  override render() {
    if (this.rows_.length === 0) {
      return html`<div class="empty">No sessions yet.</div>`;
    }
    return html`
      ${this.rows_.map(
          row => html`
            <div class="row${row.running ? ' row--running' : ''}">
              <span class="id" title=${row.sessionId}>${row.sessionId}</span>
              <span class="dot${row.running ? ' dot--running' : ''}"
                  aria-label=${row.running ? 'running' : 'idle'}></span>
              ${row.tool > 0 ? html`
                <span title="tool : success / total">tool : <code>${
                    Math.max(0, row.tool - row.toolFail)}/${row.tool}</code></span>
              ` : nothing}
              ${row.mcp > 0 ? html`
                <span title="mcp : success / total">mcp : <code>${
                    Math.max(0, row.mcp - row.mcpFail)}/${row.mcp}</code></span>
              ` : nothing}
            </div>
          `)}
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'nexus-session-list': NexusSessionList;
  }
}

customElements.define(NexusSessionList.is, NexusSessionList);
