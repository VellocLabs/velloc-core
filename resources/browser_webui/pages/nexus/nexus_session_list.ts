// Copyright 2025 The Custom Browser Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {addWebUiListener, removeWebUiListener} from 'chrome://resources/js/cr.js';
import type {WebUiListener} from 'chrome://resources/js/cr.js';
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

const SUMMARY_EVENT = 'nexusSessionsSummary';

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
  private listener_: WebUiListener|null = null;

  constructor() {
    super();
    // Mirrors `nexus_topbar.ts`: register early so we don't lose the
    // first broadcast that the SharedWorker manager fires on connect.
    this.listener_ = addWebUiListener(
        SUMMARY_EVENT,
        (payload: NexusSessionsSummaryPayload) => this.onSummary_(payload));
  }

  override disconnectedCallback(): void {
    if (this.listener_) {
      removeWebUiListener(this.listener_);
      this.listener_ = null;
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
