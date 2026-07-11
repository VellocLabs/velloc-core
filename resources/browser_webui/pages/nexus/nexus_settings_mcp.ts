// Copyright 2025 The Custom Browser Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// TODO(nexus-mcp): wire to a real MCP server registry. This pass is a
// visual-only stub — agent-behavior toggles, server toggles, and Add /
// Import / Edit / Remove actions do not persist or call any backend. Mock
// data lives in MOCK_SERVERS below.

import 'chrome://resources/cr_elements/cr_button/cr_button.js';
import 'chrome://resources/cr_elements/cr_input/cr_input.js';
import 'chrome://resources/cr_elements/cr_toggle/cr_toggle.js';

import {
  CrLitElement,
  css,
  html,
  nothing,
  type CSSResultGroup,
} from 'chrome://resources/lit/v3_0/lit.rollup.js';

import {getString, paneStyles} from './nexus_shared.js';
import './nexus_subpage_layout.js';

type ServerStatus = 'connected'|'auth-required'|'disconnected'|'error';

interface McpServer {
  builtin: boolean;
  enabled: boolean;
  id: string;
  name: string;
  status: ServerStatus;
  target: string;
  tools: number;
  transport: 'stdio'|'http';
}

const MOCK_SERVERS: McpServer[] = [
  {
    id: 'fs',
    name: 'Filesystem',
    transport: 'stdio',
    target: 'npx -y @modelcontextprotocol/server-filesystem ~/Documents',
    tools: 8,
    status: 'connected',
    enabled: true,
    builtin: true,
  },
  {
    id: 'github',
    name: 'GitHub',
    transport: 'stdio',
    target: 'npx -y @modelcontextprotocol/server-github',
    tools: 14,
    status: 'connected',
    enabled: true,
    builtin: false,
  },
  {
    id: 'postgres',
    name: 'Postgres (prod)',
    transport: 'stdio',
    target: 'npx -y @modelcontextprotocol/server-postgres',
    tools: 5,
    status: 'auth-required',
    enabled: true,
    builtin: false,
  },
  {
    id: 'linear',
    name: 'Linear',
    transport: 'http',
    target: 'https://mcp.linear.app/sse',
    tools: 11,
    status: 'disconnected',
    enabled: false,
    builtin: false,
  },
  {
    id: 'slack',
    name: 'Slack',
    transport: 'stdio',
    target: 'npx -y @modelcontextprotocol/server-slack',
    tools: 9,
    status: 'error',
    enabled: true,
    builtin: false,
  },
];

const DEFAULT_SYSTEM_PROMPT =
    'You are Nexus, an agentic browsing assistant. Use the connected MCP ' +
    'tools to ground answers in real systems before responding.';

export class NexusSettingsMcpElement extends CrLitElement {
  static get is() {
    return 'nexus-settings-mcp';
  }

  static override styles: CSSResultGroup = [
    paneStyles,
    css`
      :host {
        width: 100%;
      }

      .nexus-input {
        --cr-input-width: 100%;

        display: block;
        width: 100%;
      }

      .max-steps-input {
        --cr-input-width: 96px;
        max-width: 96px;
      }

      .system-prompt-textarea {
        background: var(
            --md-background-color,
            var(--cr-card-background-color, var(--app-surface)));
        border: 1px solid var(--cr-separator-color);
        border-radius: 12px;
        box-sizing: border-box;
        color: var(--cr-primary-text-color, var(--text-primary));
        font-family: inherit;
        font-size: inherit;
        line-height: 1.5;
        min-height: 64px;
        outline: none;
        padding: 12px 14px;
        resize: vertical;
        transition: border-color .12s, box-shadow .12s;
        width: 100%;
      }
      .system-prompt-textarea:focus {
        border-color: var(--cr-focus-outline-color);
        box-shadow: 0 0 0 1px var(--cr-focus-outline-color);
      }

      .servers-header {
        display: flex;
        align-items: center;
        gap: 8px;
        margin-bottom: 12px;
      }
      .servers-header h3 {
        flex: 1;
        font-size: 1rem;
        font-weight: 500;
        margin: 0;
        color: var(--cr-primary-text-color, var(--text-primary));
      }

      .server-row {
        align-items: flex-start;
      }

      .server-status-dot {
        align-items: center;
        border-radius: 999px;
        display: grid;
        flex-shrink: 0;
        height: 24px;
        margin-inline-end: 16px;
        margin-top: 4px;
        place-items: center;
        width: 24px;
        font-size: 14px;
      }
      .server-status-dot.connected {
        background: var(--google-green-50, #e6f4ea);
        color: var(--google-green-700, #137333);
      }
      .server-status-dot.auth-required {
        background: var(--google-yellow-50, #fef7e0);
        color: var(--google-yellow-900, #b06000);
      }
      .server-status-dot.disconnected {
        background: var(--md-background-color, #eef0f4);
        color: var(--cr-secondary-text-color, var(--text-secondary));
      }
      .server-status-dot.error {
        background: var(--google-red-50, #fce8e6);
        color: var(--google-red-700, #c5221f);
      }

      .server-name-row {
        align-items: center;
        display: flex;
        flex-wrap: wrap;
        gap: 8px;
        margin-bottom: 2px;
      }
      .transport-tag {
        background: var(--md-background-color, #eef0f4);
        border-radius: 3px;
        color: var(--cr-secondary-text-color, var(--text-secondary));
        font-family: ui-monospace, SFMono-Regular, "SF Mono", Menlo, Consolas,
                     monospace;
        font-size: 10px;
        font-weight: 600;
        letter-spacing: 0.06em;
        padding: 1px 6px;
      }
      .builtin-tag {
        background: var(--google-blue-50);
        border-radius: 3px;
        color: var(--google-blue-700);
        font-size: 10px;
        font-weight: 500;
        padding: 1px 6px;
      }
      .target-text {
        color: var(--cr-secondary-text-color, var(--text-secondary));
        font-family: ui-monospace, SFMono-Regular, "SF Mono", Menlo, Consolas,
                     monospace;
        font-size: 11.5px;
        margin-bottom: 4px;
        word-break: break-all;
      }
      .meta-text { font-size: 11.5px; }
      .meta-ok    { color: var(--google-green-700, #137333); }
      .meta-warn  { color: var(--google-yellow-900, #b06000); }
      .meta-muted { color: var(--cr-secondary-text-color, var(--text-secondary)); }
      .meta-err   { color: var(--google-red-700, #c5221f); }

      .row-actions {
        align-items: center;
        display: flex;
        gap: 4px;
      }

      .help-link {
        color: var(--cr-link-color);
        text-decoration: none;
      }
      .help-link:hover { text-decoration: underline; }
    `,
  ];

  private autoApprove_ = false;
  private parallelTools_ = true;
  private maxSteps_ = 40;
  private systemPrompt_ = DEFAULT_SYSTEM_PROMPT;
  private servers_: McpServer[] = [...MOCK_SERVERS];

  private onAutoApproveChange_(e: CustomEvent<boolean>) {
    this.autoApprove_ = e.detail;
    this.requestUpdate();
  }

  private onParallelChange_(e: CustomEvent<boolean>) {
    this.parallelTools_ = e.detail;
    this.requestUpdate();
  }

  private onMaxStepsInput_(e: Event) {
    const v = Number((e.target as HTMLInputElement).value);
    if (Number.isFinite(v)) {
      this.maxSteps_ = Math.max(1, Math.min(200, v));
      this.requestUpdate();
    }
  }

  private onSystemPromptInput_(e: Event) {
    this.systemPrompt_ = (e.target as HTMLTextAreaElement).value;
  }

  private onServerToggle_(id: string, e: CustomEvent<boolean>) {
    this.servers_ = this.servers_.map(
        s => s.id === id ? {...s, enabled: e.detail} : s);
    this.requestUpdate();
  }

  private onRemoveServer_(id: string) {
    this.servers_ = this.servers_.filter(s => s.id !== id);
    this.requestUpdate();
  }

  private renderStatusDot_(status: ServerStatus) {
    let glyph = '';
    switch (status) {
      case 'connected': glyph = '✓'; break;
      case 'auth-required': glyph = '🔒'; break;
      case 'disconnected': glyph = '○'; break;
      case 'error': glyph = '!'; break;
    }
    return html`<div class="server-status-dot ${status}">${glyph}</div>`;
  }

  private renderStatusMeta_(server: McpServer) {
    switch (server.status) {
      case 'connected':
        return html`<span class="meta-text meta-ok">${
            server.tools} tools available</span>`;
      case 'auth-required':
        return html`<span class="meta-text meta-warn">Authentication required</span>`;
      case 'disconnected':
        return html`<span class="meta-text meta-muted">Disconnected</span>`;
      case 'error':
        return html`<span class="meta-text meta-err">Connection error · last seen 14 min ago</span>`;
    }
  }

  private renderServerActions_(server: McpServer) {
    return html`
      <div class="row-actions">
        ${server.status === 'auth-required'
            ? html`<cr-button>Authorize</cr-button>` : nothing}
        ${server.status === 'error'
            ? html`<cr-button>Retry</cr-button>` : nothing}
        <cr-button>Edit</cr-button>
        ${!server.builtin
            ? html`<cr-button @click=${() => this.onRemoveServer_(server.id)}>
                     Remove
                   </cr-button>`
            : nothing}
        <cr-toggle
            ?checked=${server.enabled}
            @change=${(e: CustomEvent<boolean>) =>
                this.onServerToggle_(server.id, e)}>
        </cr-toggle>
      </div>
    `;
  }

  private connectedCount_(): number {
    return this.servers_.filter(
        s => s.enabled && s.status === 'connected').length;
  }

  private toolsAvailable_(): number {
    return this.servers_.filter(s => s.enabled).reduce(
        (n, s) => n + (s.status === 'connected' ? s.tools : 0), 0);
  }

  protected override render() {
    return html`
      <nexus-subpage-layout>
        <section class="panel-shell">
          <!-- Agent behavior -->
          <section class="settings-page-section">
            <div class="settings-page-section-header">
              <h2 class="settings-page-section-title">${getString(
                  'customBrowserNexusMcpPageTitle', 'Agent · MCP')}</h2>
            </div>
            <div class="section-card">
              <div class="card-row">
                <div class="card-row-content">
                  <div class="card-row-title">Auto-approve tool calls</div>
                  <div class="card-row-description">
                    Skip the per-call confirmation dialog. Recommended only
                    for trusted tools.
                  </div>
                </div>
                <cr-toggle class="card-row-action"
                    ?checked=${this.autoApprove_}
                    @change=${this.onAutoApproveChange_}>
                </cr-toggle>
              </div>
              <div class="card-row">
                <div class="card-row-content">
                  <div class="card-row-title">Parallel tool execution</div>
                  <div class="card-row-description">
                    Run independent tool calls concurrently for faster
                    responses.
                  </div>
                </div>
                <cr-toggle class="card-row-action"
                    ?checked=${this.parallelTools_}
                    @change=${this.onParallelChange_}>
                </cr-toggle>
              </div>
              <div class="card-row">
                <div class="card-row-content">
                  <div class="card-row-title">Max agent steps</div>
                  <div class="card-row-description">
                    Hard ceiling on the agent's tool-call loop per turn.
                  </div>
                </div>
                <cr-input class="card-row-action max-steps-input"
                    type="number"
                    .value=${String(this.maxSteps_)}
                    min="1" max="200"
                    @input=${this.onMaxStepsInput_}>
                </cr-input>
              </div>
              <div class="card-row block">
                <div class="field-label with-description">
                  System prompt
                </div>
                <p class="card-row-description field-description">
                  Prepended to every conversation. Keep it short.
                </p>
                <textarea class="system-prompt-textarea" rows="3"
                    .value=${this.systemPrompt_}
                    @input=${this.onSystemPromptInput_}>
                </textarea>
              </div>
            </div>
          </section>

          <!-- Servers -->
          <section class="settings-page-section">
            <div class="servers-header">
              <h3>MCP Servers</h3>
              <cr-button>Import JSON</cr-button>
              <cr-button class="action-button">Add server</cr-button>
            </div>
            <p class="card-row-description"
                style="margin: 0 0 12px;">
              ${this.connectedCount_()} of ${this.servers_.length} connected
              · ${this.toolsAvailable_()} tools available
            </p>
            <div class="section-card">
              ${this.servers_.map(server => html`
                <div class="card-row block server-row">
                  <div style="display: flex; gap: 0; align-items: flex-start;">
                    ${this.renderStatusDot_(server.status)}
                    <div style="flex: 1; min-width: 0;">
                      <div class="server-name-row">
                        <div class="card-row-title">${server.name}</div>
                        <span class="transport-tag">${
                            server.transport.toUpperCase()}</span>
                        ${server.builtin
                            ? html`<span class="builtin-tag">Built-in</span>`
                            : nothing}
                      </div>
                      <div class="target-text">${server.target}</div>
                      <div>${this.renderStatusMeta_(server)}</div>
                      <div style="margin-top: 8px;">
                        ${this.renderServerActions_(server)}
                      </div>
                    </div>
                  </div>
                </div>
              `)}
            </div>
            <p class="helper-text" style="margin-top: 12px;">
              ${getString('customBrowserProductName', 'Nexus')} follows the
              <a class="help-link" href="#"
                  @click=${(e: Event) => e.preventDefault()}>Model Context Protocol</a>
              spec. Servers run locally over stdio or remotely over HTTP/SSE.
            </p>
          </section>
        </section>
      </nexus-subpage-layout>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'nexus-settings-mcp': NexusSettingsMcpElement;
  }
}

customElements.define(
    NexusSettingsMcpElement.is, NexusSettingsMcpElement);
