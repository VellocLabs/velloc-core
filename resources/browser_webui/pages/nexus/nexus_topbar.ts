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

import '../../components/native/native_view_aliases.js';
import './nexus_quick_settings.js';
import {uiStore} from '../../ui_store.js';
import {UIMode} from '../../ui_view_controller.mojom-webui.js';
import {
  browserModeIcon,
  globeIcon,
  globeOffIcon,
  shieldIcon,
} from './nexus_icons.js';
import type {NexusSettingsSection} from './nexus_shared.js';

export type NexusTopbarSurfaceMode = 'nexus-view'|'settings'|'warning';

const PERMISSIONS_TOTAL = 5;

// Wire format for `nexusAgentStateChanged`. Two C++ producers fire this
// event with different subsets of fields; the topbar only updates the
// fields actually present in each payload, so producers stay decoupled.
//   - `ToolsAdapter::BroadcastAgentState`     — `running` (a tool is in
//     flight), `toolName`, `isBrowserTool`, `successful`, `total`.
//   - `NexusLlmBridgeAdapter::BroadcastModelBusyIfChanged` — `modelBusy`
//     (a model.call is in flight or any stream is active).
// Render priority is `running` > `modelBusy` > idle, so a tool dispatched
// during model streaming surfaces as `running : <tool>` rather than
// `thinking`.
export interface NexusAgentStatePayload {
  running?: boolean;
  toolName?: string;
  isBrowserTool?: boolean;
  successful?: number;
  total?: number;
  modelBusy?: boolean;
}

export interface NexusPermissionsPayload {
  granted: number;
  total?: number;
}

// IPC events the topbar listens for. Names must match what the C++ side
// dispatches via FireWebUIListener("..."). Mirrors the
// 'nexusDismissBootWelcome' pattern in nexus_view.ts (registered in the
// constructor, not firstUpdated, so we win the C++ broadcast race).
const AGENT_STATE_EVENT = 'nexusAgentStateChanged';
const PERMISSIONS_EVENT = 'nexusPermissionsChanged';

export class NexusTopbar extends CrLitElement {
  static get is() {
    return 'nexus-topbar';
  }

  static override properties = {
    surfaceMode: {type: String},
    agentZoneVisible: {type: Boolean},
  };

  // Public reactive props set by parent.
  declare surfaceMode: NexusTopbarSurfaceMode;
  declare agentZoneVisible: boolean;

  // Internal state.
  private agentRunning_ = false;
  private agentModelBusy_ = false;
  private agentToolName_ = '';
  private agentIsBrowserTool_ = false;
  // Last value of `agentRunning_ && agentIsBrowserTool_`. Used to fire
  // `nexus-show-agent-zone-transient` only on the false → true edge, so
  // each browser-tool dispatch triggers exactly one show — no paired
  // hide when the tool ends.
  private lastBrowserToolRunning_ = false;
  private toolsSuccessful_ = 0;
  private toolsTotal_ = 0;
  // Whether the permissions badge has received any real data yet. The
  // C++ broadcaster for `nexusPermissionsChanged` is not wired, so until
  // someone calls `setPermissionsCount` (or that event arrives) the
  // badge would otherwise show a meaningless `5/5` default. Hide it
  // until initialized.
  private permissionsInitialized_ = false;
  private permissionsGranted_ = PERMISSIONS_TOTAL;
  private permissionsTotal_ = PERMISSIONS_TOTAL;

  // WebUI listener handles — owned for the lifetime of the element.
  private agentStateListener_: WebUiListener|null = null;
  private permissionsListener_: WebUiListener|null = null;

  static override styles: CSSResultGroup = css`
    :host {
      /* Design tokens — scoped to this component, not :root. Verbatim hex
       * values from nexus/project/styles.css to keep pixel-perfect identity. */
      --chrome-frame: #eef0f4;
      --chrome-toolbar: #ffffff;
      --chrome-canvas: #f8f9fc;
      --chrome-divider: #e5e7eb;
      --chrome-divider-strong: #d6dae0;

      --ink-1: #1f2024;
      --ink-2: #4b5159;
      --ink-3: #707682;
      --ink-4: #9aa0a8;

      --hover: rgba(15, 23, 42, 0.05);
      --accent: #1a73e8;
      --accent-soft: #e8f0fe;
      --accent-soft-2: #d2e3fc;

      --success: #137333;
      --danger: #c5221f;

      --radius-md: 8px;
      --radius-pill: 999px;

      --font: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto,
              "Helvetica Neue", Arial, sans-serif;
      --font-mono: "SF Mono", Menlo, Consolas, "Roboto Mono", monospace;

      align-items: center;
      background: var(--chrome-toolbar);
      border-bottom: 1px solid var(--chrome-divider);
      box-sizing: border-box;
      color: var(--ink-1);
      display: flex;
      font-family: var(--font);
      font-size: 13px;
      gap: 6px;
      height: 41px;
      overflow: visible;
      position: relative;
      width: 100%;
      -webkit-app-region: drag;
    }

    :host > * {
      -webkit-app-region: no-drag;
    }

    /* Drag strip between status cluster and the native window controls.
     * Stays draggable so the user can grab the right portion of the header. */
    .header__drag {
      align-self: stretch;
      flex-shrink: 0;
      width: 12px;
      -webkit-app-region: drag;
    }

    /* Generic header icon button. */
    .hbtn {
      align-items: center;
      background: transparent;
      border: 0;
      border-radius: var(--radius-md);
      color: var(--ink-2);
      cursor: pointer;
      display: inline-flex;
      font: inherit;
      gap: 8px;
      height: 32px;
      min-width: 32px;
      padding: 0 8px;
      position: relative;
      transition: background .12s, color .12s;
      white-space: nowrap;
    }
    .hbtn:hover { background: var(--hover); color: var(--ink-1); }
    .hbtn--active { background: var(--accent-soft); color: var(--accent); }
    .hbtn--active:hover {
      background: var(--accent-soft-2);
      color: var(--accent);
    }
    /* Icons inherit their viewBox-intrinsic size (24x24) per the design.
     * The design has no explicit .icon size rule for the header, and Chrome
     * renders inline SVG with viewBox at viewBox-pixel dimensions. */

    .header__divider {
      background: var(--chrome-divider-strong);
      height: 20px;
      margin: 0 4px;
      width: 1px;
    }
    .header__spacer { flex: 1; }
    .header__sep {
      align-self: stretch;
      background: var(--ink-4);
      margin: 6px 6px;
      opacity: 0.35;
      width: 1px;
    }

    /* Center status pill. */
    .header__status {
      align-items: center;
      backdrop-filter: blur(8px);
      background: rgba(255, 255, 255, 0.6);
      border: 1px solid var(--chrome-divider);
      border-radius: var(--radius-pill);
      box-shadow: inset 0 1px 0 rgba(255, 255, 255, 0.5);
      color: var(--ink-2);
      display: inline-flex;
      font-size: 11.5px;
      gap: 8px;
      height: 30px;
      letter-spacing: -0.005em;
      padding: 0 14px;
    }
    .header__status code {
      color: var(--ink-2);
      font-family: var(--font-mono);
    }
    .dot {
      background: var(--ink-4);
      border-radius: 50%;
      flex-shrink: 0;
      height: 6px;
      width: 6px;
    }
    .dot--blue {
      animation: pulse-blue 1.6s infinite;
      background: var(--accent);
    }
    @keyframes pulse-blue {
      0%   { box-shadow: 0 0 0 0 rgba(26, 115, 232, .5); }
      70%  { box-shadow: 0 0 0 5px rgba(26, 115, 232, 0); }
      100% { box-shadow: 0 0 0 0 rgba(26, 115, 232, 0); }
    }
    /* Amber dot — model is generating but no tool is dispatched yet
     * ("thinking" phase). Distinct hue from blue so the user can tell
     * at a glance whether they're waiting on the model vs a tool. */
    .dot--amber {
      animation: pulse-amber 1.6s infinite;
      background: #f59e0b;
    }
    @keyframes pulse-amber {
      0%   { box-shadow: 0 0 0 0 rgba(245, 158, 11, .5); }
      70%  { box-shadow: 0 0 0 5px rgba(245, 158, 11, 0); }
      100% { box-shadow: 0 0 0 0 rgba(245, 158, 11, 0); }
    }

    /* Permissions badge — passive count, click navigates to settings. */
    .perm-badge {
      align-items: center;
      background: transparent;
      border: 0;
      border-left: 1px solid var(--chrome-divider);
      border-radius: 6px;
      color: var(--ink-3);
      cursor: pointer;
      display: inline-flex;
      font: inherit;
      font-size: 11px;
      gap: 6px;
      margin-left: 10px;
      padding: 2px 6px 2px 10px;
    }
    .perm-badge:hover { background: var(--hover); }
    .perm-badge__count {
      color: var(--ink-2);
      font-weight: 600;
    }
    .perm-badge--danger { color: var(--danger); }
    .perm-badge--danger .perm-badge__count { color: var(--danger); }

    /* System-aligned button (browser-mode switch). Sits flush right at the
     * native window-controls height. */
    .sysbtn {
      align-self: stretch;
      background: transparent;
      border: 0;
      color: var(--ink-2);
      cursor: pointer;
      display: grid;
      height: 100%;
      place-items: center;
      transition: background .12s, color .12s;
      width: 44px;
    }
    .sysbtn:hover { background: var(--hover); color: var(--ink-1); }

    /* Hairline loading bar — only when agent is running. */
    .header__loadbar {
      animation: nexus-prog 1.2s infinite linear;
      background: linear-gradient(90deg, transparent, var(--accent), transparent);
      background-repeat: no-repeat;
      background-size: 40% 100%;
      bottom: -1px;
      height: 2px;
      left: 0;
      position: absolute;
      right: 0;
    }
    @keyframes nexus-prog {
      0%   { background-position: -40% 0; }
      100% { background-position: 140% 0; }
    }

    /* Native window controls and browser-button are 37px tall C++ Views;
     * vertically center them in the 40px header. */
    caption-browser-button,
    caption-button-container {
      align-self: center;
    }
  `;

  constructor() {
    super();
    this.surfaceMode = 'nexus-view';
    this.agentZoneVisible = false;

    // Register WebUI listeners as early as possible — before the element
    // is even attached — so a fast C++ broadcast cannot race past us.
    // Mirrors the `nexusDismissBootWelcome` pattern in nexus_view.ts:177.
    // The C++ broadcaster (TODO) is not yet implemented; until then,
    // these listeners are dormant and callers drive state via the public
    // setters below (or DevTools / the React guest using sendWithPromise
    // proxies, when those land).
    this.agentStateListener_ = addWebUiListener(
        AGENT_STATE_EVENT,
        (payload: NexusAgentStatePayload) =>
            this.onAgentStateChanged_(payload));
    this.permissionsListener_ = addWebUiListener(
        PERMISSIONS_EVENT,
        (payload: NexusPermissionsPayload) =>
            this.onPermissionsChanged_(payload));
  }

  override disconnectedCallback(): void {
    if (this.agentStateListener_) {
      removeWebUiListener(this.agentStateListener_);
      this.agentStateListener_ = null;
    }
    if (this.permissionsListener_) {
      removeWebUiListener(this.permissionsListener_);
      this.permissionsListener_ = null;
    }
    super.disconnectedCallback();
  }

  // --- IPC handlers ----------------------------------------------------

  private onAgentStateChanged_(payload: NexusAgentStatePayload): void {
    if (!payload) {
      return;
    }
    let touched = false;
    if (typeof payload.running === 'boolean') {
      this.agentRunning_ = payload.running;
      touched = true;
    }
    if (typeof payload.modelBusy === 'boolean') {
      this.agentModelBusy_ = payload.modelBusy;
      touched = true;
    }
    if (typeof payload.toolName === 'string') {
      this.agentToolName_ = payload.toolName;
      touched = true;
    }
    if (typeof payload.isBrowserTool === 'boolean') {
      this.agentIsBrowserTool_ = payload.isBrowserTool;
      touched = true;
    }
    if (typeof payload.successful === 'number') {
      this.toolsSuccessful_ = Math.max(0, payload.successful);
      touched = true;
    }
    if (typeof payload.total === 'number') {
      this.toolsTotal_ = Math.max(0, payload.total);
      touched = true;
    }
    if (touched) {
      this.maybeDispatchBrowserToolTransient_();
      this.requestUpdate();
    }
  }

  // Fires `nexus-show-agent-zone-transient` once on the leading edge of
  // every browser-category tool dispatch. The parent applies a one-shot
  // "show" — it does NOT mutate the saved state, and there's no paired
  // hide event when the tool ends. The zone therefore stays visible
  // until the user manually clicks the globe (the only thing that
  // updates the saved state, via `nexus-toggle-agent-zone`).
  private maybeDispatchBrowserToolTransient_() {
    const browserToolRunning =
        this.agentRunning_ && this.agentIsBrowserTool_;
    const wasRunning = this.lastBrowserToolRunning_;
    this.lastBrowserToolRunning_ = browserToolRunning;
    if (!wasRunning && browserToolRunning) {
      this.dispatchEvent(new CustomEvent('nexus-show-agent-zone-transient', {
        bubbles: true,
        composed: true,
      }));
    }
  }

  private onPermissionsChanged_(payload: NexusPermissionsPayload): void {
    if (!payload || typeof payload.granted !== 'number') {
      return;
    }
    this.setPermissionsCount(
        payload.granted, payload.total ?? PERMISSIONS_TOTAL);
  }

  // --- Public API ------------------------------------------------------

  setPermissionsCount(granted: number, total: number = PERMISSIONS_TOTAL): void {
    this.permissionsGranted_ = Math.max(0, Math.min(granted, total));
    this.permissionsTotal_ = total;
    this.permissionsInitialized_ = true;
    this.requestUpdate();
  }

  // --- Click handlers --------------------------------------------------

  // Globe button — drives agent-zone visibility. Click dispatches a toggle
  // event; the parent (<nexus-page>) flips <nexus-view>'s flag and pushes
  // the new value back to us via the `agentZoneVisible` reactive prop, so
  // the icon (globe vs globe-off) and `hbtn--active` state stay in sync
  // with reality without local mirror state.
  private onAgentZoneToggleClick_() {
    this.dispatchEvent(new CustomEvent(
        'nexus-toggle-agent-zone', {bubbles: true, composed: true}));
  }

  private onPermissionsBadgeClick_() {
    const detail: {section: NexusSettingsSection} = {section: 'permissions'};
    this.dispatchEvent(new CustomEvent(
        'nexus-open-settings', {bubbles: true, composed: true, detail}));
  }

  // Sysbtn — switches the entire UI to plain Chrome (UIMode.kStandard).
  // Goes through uiStore so BATCH.setPaused() and observers fire.
  private onSwitchToStandardClick_() {
    uiStore.setMode(UIMode.kStandard);
    // Intentionally nothing else — <nexus-page> is about to be torn down.
  }

  // --- Render ----------------------------------------------------------

  override render() {
    return html`
      ${this.surfaceMode === 'warning'
          ? nothing
          : html`<nexus-quick-settings></nexus-quick-settings>
                 <div class="header__divider"></div>`}
      ${this.surfaceMode === 'warning' ? nothing : this.renderCenterCluster_()}
      <div class="header__sep"></div>
      ${this.surfaceMode === 'warning' ? nothing :
        html`<button class="sysbtn"
                @click=${this.onSwitchToStandardClick_}
                title="Switch to browser mode"
                aria-label="Switch to browser mode">
              ${browserModeIcon}
            </button>`}
      <div class="header__drag"></div>
      <caption-browser-button align-x="right"></caption-browser-button>
      <caption-button-container align-x="right"></caption-button-container>
      ${(this.agentRunning_ || this.agentModelBusy_) &&
            this.surfaceMode === 'nexus-view'
          ? html`<div class="header__loadbar"></div>`
          : nothing}
    `;
  }

  private renderCenterCluster_() {
    const showPermBadge =
        this.surfaceMode === 'nexus-view' && this.permissionsInitialized_;
    const liveSurface = this.surfaceMode === 'nexus-view';
    const showRunning = liveSurface && this.agentRunning_;
    const showThinking =
        liveSurface && !this.agentRunning_ && this.agentModelBusy_;
    const toolsSegment = this.toolsTotal_ > 0
        ? html` · tools : <code>${this.toolsSuccessful_}/${this.toolsTotal_}</code>`
        : nothing;
    let statusBody;
    if (showRunning) {
      statusBody = html`<span class="dot dot--blue"></span> Agent · running${
          this.agentToolName_
              ? html` : <code>${this.agentToolName_}</code>`
              : nothing}${toolsSegment}`;
    } else if (showThinking) {
      statusBody = html`<span class="dot dot--amber"></span> Agent · thinking${
          toolsSegment}`;
    } else {
      statusBody = html`<span class="dot"></span> Agent · idle${toolsSegment}`;
    }
    return html`
      <button
          class="hbtn${this.agentZoneVisible ? ' hbtn--active' : ''}"
          @click=${this.onAgentZoneToggleClick_}
          title=${this.agentZoneVisible ? 'Hide agent zone' :
                                            'Show agent zone'}
          aria-label=${this.agentZoneVisible ? 'Hide agent zone' :
                                                'Show agent zone'}>
        ${this.agentZoneVisible ? globeIcon : globeOffIcon}
      </button>
      <div class="header__spacer"></div>
      <div class="header__status">
        ${statusBody}
        ${showPermBadge ? this.renderPermissionsBadge_() : nothing}
      </div>
      <div class="header__spacer"></div>
    `;
  }

  private renderPermissionsBadge_() {
    const danger = this.permissionsGranted_ === 0;
    return html`
      <button
          class="perm-badge${danger ? ' perm-badge--danger' : ''}"
          @click=${this.onPermissionsBadgeClick_}
          title=${`Permissions · ${this.permissionsGranted_}/${
              this.permissionsTotal_} granted`}>
        ${shieldIcon}
        <span class="perm-badge__count">${this.permissionsGranted_}/${
        this.permissionsTotal_}</span>
      </button>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'nexus-topbar': NexusTopbar;
  }
}

customElements.define(NexusTopbar.is, NexusTopbar);
