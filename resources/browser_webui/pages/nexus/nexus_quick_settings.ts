// Copyright 2025 The Custom Browser Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {addWebUiListener, removeWebUiListener, sendWithPromise} from 'chrome://resources/js/cr.js';
import type {WebUiListener} from 'chrome://resources/js/cr.js';
import {
  CrLitElement,
  css,
  html,
  nothing,
  type CSSResultGroup,
} from 'chrome://resources/lit/v3_0/lit.rollup.js';

import {getString} from './nexus_shared.js';

// ---------------------------------------------------------------------------
// Wire-format mirrors
// ---------------------------------------------------------------------------
//
// `getNexusCatalog` returns the enriched popup catalog: each provider entry
// carries identity (id, label, iconSvg), verification + config state, the
// user's enabled-models allowlist, and the curated model list pulled from
// the TS-pushed NEXUS_MODEL_CONFIG. The shape is defined by
// `NexusSettingsHandler::BuildEnrichedNexusCatalog_` — keep in sync.

interface EnrichedConfiguredModel {
  model: string;
  name?: string;
  defaultThinkingTier?: string;
  availableThinkingTiers?: string[];
  contextWindow?: number;
  maxOutputTokens?: number;
}

interface EnrichedProvider {
  providerId: string;
  label?: string;
  iconSvg?: string;
  isConfigured?: boolean;
  isVerified?: boolean;
  enabledModels?: string[];
  filterMode?: 'curated'|'passthrough';
  models?: EnrichedConfiguredModel[];
}

interface EnrichedCatalog {
  providers?: EnrichedProvider[];
  activeProviderId?: string;
}

interface SessionSelection {
  providerId: string;
  modelId: string;
  thinkingTier?: string;
}

interface SessionStatus {
  sessionId?: string;
  selection?: SessionSelection;
}

// Mirror of the unified topbar feed payload. Defined inline rather than
// shared because the in-browser WebUI build cannot import from
// `nexus_topbar.ts` cleanly — both components are siblings under the
// same surface and only need the subset of fields they consume.
type NexusCurrentSessionWirePayload =
  | {
      kind: 'snapshot';
      sessionId: string;
      model: SessionSelection|null;
      phase: 'idle'|'thinking'|'tool'|'mcp';
      active: {name: string; actionType?: string}|null;
      counters: {tool: number; toolFail: number; mcp: number; mcpFail: number};
    }
  | {
      kind: 'delta';
      sessionId: string;
      model?: SessionSelection|null;
      phase?: 'idle'|'thinking'|'tool'|'mcp';
      active?: {name: string; actionType?: string}|null;
      counters?: Partial<{
        tool: number;
        toolFail: number;
        mcp: number;
        mcpFail: number;
      }>;
    };

interface PickFailedPayload {
  providerId?: string;
  modelId?: string;
  errorCode?: string;
  error?: string;
}

// Wire shape of the `pickNexusSelection` chrome.send response. Defined as
// a named interface (rather than an inline `as { ... }` cast on the await
// expression) because the WebUI's preprocessed-tsc step fails to parse a
// multi-line inline object type after `as` on an `await`-postfixed
// expression — the parser drops back into type-literal mode and then
// misreads the rest of the function body as nested members, producing a
// cascade of TS1003/TS7006/TS2339 errors anchored at the next `if`.
interface PickNexusSelectionResult {
  ok: boolean;
  errorCode?: string;
  error?: string;
  pusherInvoked?: boolean;
  requestId?: string;
}

// One row in the dropdown list, ready to render.
interface PickerOption {
  providerId: string;
  providerLabel: string;
  iconSvg: string;
  modelId: string;       // canonical id sent to C++
  modelLabel: string;    // display title (e.g. "Claude Opus 4.7")
  defaultThinkingTier?: string;
  availableThinkingTiers?: string[];
}

// Strip the anchors + optional date suffixes that
// `nexus_web/src/browser/config/nexusModelConfig/patterns.ts` adds, then
// pass plain ids through. Returns undefined for genuinely regex-only
// patterns so we can skip rows the popup can't act on.
function deriveModelId(pattern: string|undefined): string|undefined {
  if (!pattern) return undefined;
  let stripped = pattern.replace(/^\^/, '').replace(/\$$/, '');
  stripped = stripped.replace(/\(\?:-\\d\{4\}-\\d\{2\}-\\d\{2\}\)\?$/, '');
  stripped = stripped.replace(/\(\?:-\\d\{8\}\)\?$/, '');
  if (/[()[\]{}|?+*]/.test(stripped)) return undefined;
  return stripped.replace(/\\(.)/g, '$1');
}

function isModelEnabled(
    enabledModels: string[]|undefined, modelId: string): boolean {
  if (!enabledModels || enabledModels.length === 0) {
    // Empty / absent allowlist means "all enabled" — same semantics as
    // `IsNexusModelEnabled` on the C++ side.
    return true;
  }
  return enabledModels.includes(modelId);
}

function buildPickerOptions(catalog: EnrichedCatalog|null): PickerOption[] {
  if (!catalog?.providers) return [];
  const options: PickerOption[] = [];
  for (const provider of catalog.providers) {
    if (!provider.providerId) continue;
    // Only verified providers with a saved API key make it into the popup.
    if (!provider.isConfigured || !provider.isVerified) continue;

    const providerLabel = provider.label || provider.providerId;
    const iconSvg = provider.iconSvg ?? '';
    for (const model of provider.models ?? []) {
      const modelId = deriveModelId(model.model);
      if (!modelId) continue;
      if (!isModelEnabled(provider.enabledModels, modelId)) continue;
      options.push({
        providerId: provider.providerId,
        providerLabel,
        iconSvg,
        modelId,
        modelLabel: model.name || modelId,
        defaultThinkingTier: model.defaultThinkingTier,
        availableThinkingTiers: model.availableThinkingTiers,
      });
    }
  }
  return options;
}

// Build a `data:image/svg+xml` URL for an inline SVG string. Keeps the
// CSS background path simple compared to embedding SVG markup directly.
function iconDataUrl(svg: string|undefined): string|null {
  if (!svg) return null;
  return `data:image/svg+xml;charset=utf-8,${encodeURIComponent(svg)}`;
}

// ---------------------------------------------------------------------------
// Element
// ---------------------------------------------------------------------------

export class NexusQuickSettingsElement extends CrLitElement {
  static get is() {
    return 'nexus-quick-settings';
  }

  static override properties = {
    open_: {type: Boolean, state: true},
    query_: {type: String, state: true},
    pendingPickKey_: {type: String, state: true},
    phase_: {type: String, state: true},
  };

  // Internal state.
  private catalog_: EnrichedCatalog|null = null;
  private status_: SessionStatus|null = null;
  private options_: PickerOption[] = [];
  private declare open_: boolean;
  private declare query_: string;
  // While a pick is in flight (chrome.send sent, awaiting the next
  // status broadcast) the option key is stored here so we can render
  // a dim/disabled state on that row.
  private declare pendingPickKey_: string;
  // Active session's phase, sourced from `nexusCurrentSessionState`.
  // Drives the trigger's border-runner animation while the model is
  // thinking (no tool dispatched). Other phases — `tool` / `mcp` /
  // `idle` — leave the trigger plain; the topbar pill carries those
  // states. Typed as `string` rather than a literal union so the
  // Chromium WebUI's TS preprocessor (which doesn't support union
  // types on Lit reactive properties) accepts the declaration.
  private declare phase_: string;
  private lastError_: string|null = null;

  // WebUI listener handles — cleared in disconnectedCallback.
  private catalogListener_: WebUiListener|null = null;
  private statusListener_: WebUiListener|null = null;
  private pickFailedListener_: WebUiListener|null = null;

  // Bound document-level click listener — tracked so we can detach it.
  private outsideClickHandler_: ((event: PointerEvent) => void)|null = null;

  static override styles: CSSResultGroup = css`
    :host {
      /* Match the topbar palette so the trigger blends with the chrome. */
      --qs-bg: #ffffff;
      --qs-fg: #1f2024;
      --qs-fg-muted: #707682;
      --qs-border: #d6dae0;
      --qs-hover: rgba(15, 23, 42, 0.05);
      --qs-accent: #1a73e8;
      --qs-accent-soft: #e8f0fe;
      --qs-radius: 10px;

      align-items: center;
      display: inline-flex;
      font: inherit;
      margin-inline-start: 8px;
      position: relative;
    }

    /* --- Trigger button -------------------------------------------- */

    .qs-trigger {
      align-items: center;
      background: var(--qs-bg);
      /* Visible border at rest so the picker reads as an interactive
       * field, not just a label that lights up on hover. The hover and
       * open states promote it to the accent palette. */
      border: 1px solid var(--qs-border);
      border-radius: var(--qs-radius);
      color: var(--qs-fg);
      cursor: pointer;
      display: inline-flex;
      font: inherit;
      gap: 8px;
      height: 28px;
      min-width: 200px;
      max-width: 360px;
      padding: 0 8px 0 8px;
      transition:
          background-color 120ms ease,
          border-color 120ms ease,
          box-shadow 120ms ease;
      position: relative;
      isolation: isolate;
    }
    .qs-trigger:hover {
      background: var(--qs-hover);
      border-color: var(--qs-fg-muted);
    }
    /* Border runner — animated conic-gradient ring on the trigger
     * while the active session is in the "thinking" phase. See
     * nexus_topbar.ts for the implementation comment — same
     * overflow:hidden + oversized rotating ::before + opaque
     * ::after inner fill pattern. */
    .qs-trigger--thinking { overflow: hidden; }
    .qs-trigger--thinking::before {
      animation: qs-trigger-runner 2.8s linear infinite;
      aspect-ratio: 1;
      background: conic-gradient(
        from 0deg,
        transparent 0deg,
        transparent 270deg,
        var(--qs-accent) 330deg,
        #ffffff 350deg,
        transparent 360deg
      );
      content: "";
      left: 50%;
      pointer-events: none;
      position: absolute;
      top: 50%;
      width: 200%;
      z-index: -2;
    }
    .qs-trigger--thinking::after {
      background: var(--qs-bg);
      border-radius: var(--qs-radius);
      content: "";
      inset: 1px;
      pointer-events: none;
      position: absolute;
      z-index: -1;
    }
    .qs-trigger--thinking[aria-expanded="true"]::after {
      background: var(--qs-accent-soft);
    }
    @keyframes qs-trigger-runner {
      from { transform: translate(-50%, -50%) rotate(0deg); }
      to   { transform: translate(-50%, -50%) rotate(360deg); }
    }
    @media (prefers-reduced-motion: reduce) {
      .qs-trigger--thinking::before { animation: none; }
    }
    .qs-trigger:focus-visible {
      border-color: var(--qs-accent);
      box-shadow: 0 0 0 2px var(--qs-accent-soft);
      outline: none;
    }
    .qs-trigger[aria-expanded="true"] {
      background: var(--qs-accent-soft);
      border-color: var(--qs-accent);
      color: var(--qs-accent);
    }

    /* Icon wrapper carries no chrome — transparent fill, no border, so
     * the brand SVG (currentColor-tinted) reads as a clean glyph against
     * whatever surface the topbar happens to be on. */
    .qs-trigger__icon {
      align-items: center;
      background: transparent;
      border: 0;
      box-sizing: border-box;
      color: var(--qs-fg);
      display: inline-flex;
      flex: none;
      height: 18px;
      justify-content: center;
      width: 18px;
    }
    .qs-trigger__icon img {
      height: 100%;
      object-fit: contain;
      width: 100%;
    }
    .qs-trigger__icon-fallback {
      font-size: 10px;
      font-weight: 700;
      line-height: 1;
    }

    /* Single-line trigger: model label on the left, caret pinned to the
     * right edge of the button via margin-left:auto. Provider caption is
     * intentionally not rendered to keep the trigger compact. */
    .qs-trigger__label {
      flex: 1 1 auto;
      font-size: 13px;
      font-weight: 600;
      line-height: 16px;
      min-width: 0;
      overflow: hidden;
      text-align: start;
      text-overflow: ellipsis;
      white-space: nowrap;
    }
    .qs-trigger__label-tier {
      color: var(--qs-fg-muted);
      font-weight: 400;
    }
    .qs-trigger[aria-expanded="true"] .qs-trigger__label-tier {
      color: var(--qs-accent);
      opacity: 0.85;
    }
    .qs-trigger__caret {
      flex: none;
      height: 12px;
      margin-left: auto;
      transition: transform 150ms ease;
      width: 12px;
    }
    .qs-trigger[aria-expanded="true"] .qs-trigger__caret {
      transform: rotate(180deg);
    }

    /* --- Popup panel ----------------------------------------------- */

    .qs-popup {
      background: var(--qs-bg);
      border: 1px solid var(--qs-border);
      border-radius: var(--qs-radius);
      box-shadow: 0 10px 32px rgba(15, 23, 42, 0.14);
      box-sizing: border-box;
      left: 0;
      max-width: 380px;
      min-width: 320px;
      overflow: hidden;
      position: absolute;
      top: calc(100% + 6px);
      width: max-content;
      z-index: 1000;
    }

    .qs-search {
      border-bottom: 1px solid var(--qs-border);
      padding: 8px;
    }
    .qs-search input {
      background: var(--qs-bg);
      border: 1px solid var(--qs-border);
      border-radius: 8px;
      box-sizing: border-box;
      color: var(--qs-fg);
      font: inherit;
      font-size: 13px;
      outline: none;
      padding: 8px 10px;
      transition: border-color 120ms ease;
      width: 100%;
    }
    .qs-search input:focus {
      border-color: var(--qs-accent);
      box-shadow: 0 0 0 1px var(--qs-accent);
    }

    .qs-list {
      max-height: 360px;
      overflow-y: auto;
      padding: 4px 0;
    }

    .qs-divider {
      border-top: 1px solid var(--qs-border);
      margin: 4px 12px;
      opacity: 0.65;
    }

    .qs-row {
      align-items: center;
      background: transparent;
      border: 0;
      color: var(--qs-fg);
      cursor: pointer;
      display: flex;
      font: inherit;
      gap: 10px;
      padding: 8px 12px;
      text-align: left;
      transition: background-color 80ms ease;
      width: 100%;
    }
    .qs-row:hover { background: var(--qs-hover); }
    .qs-row[aria-selected="true"] { background: var(--qs-accent-soft); }
    .qs-row[disabled] {
      cursor: progress;
      opacity: 0.55;
    }

    .qs-row__icon {
      align-items: center;
      background: transparent;
      border: 0;
      box-sizing: border-box;
      color: var(--qs-fg);
      display: inline-flex;
      flex: none;
      height: 22px;
      justify-content: center;
      width: 22px;
    }
    .qs-row__icon img {
      height: 100%;
      object-fit: contain;
      width: 100%;
    }
    .qs-row__icon-fallback {
      font-size: 10px;
      font-weight: 700;
      line-height: 1;
    }

    .qs-row__text {
      display: flex;
      flex: 1 1 auto;
      flex-direction: column;
      gap: 1px;
      min-width: 0;
    }
    .qs-row__label {
      font-size: 13px;
      font-weight: 600;
      line-height: 16px;
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
    }
    .qs-row__caption {
      color: var(--qs-fg-muted);
      font-size: 11px;
      line-height: 14px;
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
    }

    .qs-tiers {
      display: inline-flex;
      flex: none;
      gap: 4px;
    }
    /* Tier pills are rendered as <span role="button"> (not <button>)
     * because they live inside the row <button>, and nested buttons are
     * invalid HTML — the browser auto-hoists the inner one out, which
     * silently breaks click targeting. The row click handler honours the
     * default tier; tier-pill clicks call stopPropagation so the row's
     * default-tier path doesn't override the user's explicit choice. */
    .qs-tier {
      align-items: center;
      background: transparent;
      border: 1px solid var(--qs-border);
      border-radius: 999px;
      color: var(--qs-fg-muted);
      cursor: pointer;
      display: inline-flex;
      font: inherit;
      font-size: 10px;
      letter-spacing: 0.04em;
      padding: 2px 7px;
      text-transform: uppercase;
      transition: background-color 80ms ease, color 80ms ease;
      user-select: none;
    }
    .qs-tier:hover {
      background: var(--qs-hover);
      color: var(--qs-fg);
    }
    .qs-tier:focus-visible {
      outline: 2px solid var(--qs-accent);
      outline-offset: 1px;
    }
    /* Highlight applied to whichever pill matches the row's currently
     * active tier — for the selected row that is selection.thinkingTier
     * (falling back to the catalog default when no tier was explicitly
     * picked), for every other row it is the catalog default. */
    .qs-tier--active {
      background: var(--qs-accent-soft);
      border-color: var(--qs-accent);
      color: var(--qs-accent);
    }

    .qs-check {
      align-items: center;
      background: var(--qs-accent);
      border-radius: 999px;
      color: #fff;
      display: inline-flex;
      flex: none;
      height: 16px;
      justify-content: center;
      width: 16px;
    }
    .qs-check svg {
      height: 10px;
      width: 10px;
    }

    .qs-empty {
      color: var(--qs-fg-muted);
      font-size: 13px;
      padding: 20px 16px;
      text-align: center;
    }

    .qs-error {
      background: #fff7e6;
      border-top: 1px solid var(--qs-border);
      color: #b06000;
      font-size: 12px;
      padding: 8px 12px;
    }

    .qs-footer {
      border-top: 1px solid var(--qs-border);
      padding: 4px;
    }
    .qs-footer__btn {
      align-items: center;
      background: transparent;
      border: 0;
      border-radius: 6px;
      color: var(--qs-fg);
      cursor: pointer;
      display: flex;
      font: inherit;
      font-size: 12px;
      gap: 8px;
      padding: 8px 10px;
      text-align: left;
      transition: background-color 80ms ease;
      width: 100%;
    }
    .qs-footer__btn:hover { background: var(--qs-hover); }
    .qs-footer__icon {
      flex: none;
      height: 14px;
      width: 14px;
    }
  `;

  constructor() {
    super();
    this.open_ = false;
    this.query_ = '';
    this.pendingPickKey_ = '';
    this.phase_ = 'idle';

    // Register WebUI listeners as early as possible so a fast C++
    // broadcast cannot land before we're ready (mirrors nexus_topbar.ts).
    this.catalogListener_ = addWebUiListener(
        'nexusCatalogChanged',
        (catalog: EnrichedCatalog) => this.onCatalogChanged_(catalog));
    // Subscribe to the unified per-session feed for the active model.
    // The selection lives on `payload.model` of `nexusCurrentSessionState`
    // (snapshot or delta). The previous `nexusSessionStatusChanged`
    // path was retired as part of the unified-state refactor.
    this.statusListener_ = addWebUiListener(
        'nexusCurrentSessionState',
        (payload: NexusCurrentSessionWirePayload) =>
            this.onCurrentSessionState_(payload));
    this.pickFailedListener_ = addWebUiListener(
        'nexusPickFailed',
        (payload: PickFailedPayload) => this.onPickFailed_(payload));
  }

  override connectedCallback(): void {
    super.connectedCallback();
    void this.loadInitialState_();
  }

  override disconnectedCallback(): void {
    if (this.catalogListener_) {
      removeWebUiListener(this.catalogListener_);
      this.catalogListener_ = null;
    }
    if (this.statusListener_) {
      removeWebUiListener(this.statusListener_);
      this.statusListener_ = null;
    }
    if (this.pickFailedListener_) {
      removeWebUiListener(this.pickFailedListener_);
      this.pickFailedListener_ = null;
    }
    this.detachOutsideClickHandler_();
    super.disconnectedCallback();
  }

  // --- Data loading ---------------------------------------------------

  private async loadInitialState_(): Promise<void> {
    try {
      const [catalog, status] = await Promise.all([
        sendWithPromise('getNexusCatalog') as Promise<EnrichedCatalog>,
        sendWithPromise('getNexusSessionStatus') as Promise<SessionStatus>,
      ]);
      this.catalog_ = catalog ?? null;
      this.options_ = buildPickerOptions(this.catalog_);
      this.status_ = status ?? null;
      this.maybeAutoPickDefault_();
      this.requestUpdate();
    } catch {
      // Older binary: handlers absent. Render empty.
    }
  }

  private onCatalogChanged_(catalog: EnrichedCatalog): void {
    this.catalog_ = catalog;
    this.options_ = buildPickerOptions(catalog);
    this.maybeAutoPickDefault_();
    this.requestUpdate();
  }

  private onStatusChanged_(status: SessionStatus): void {
    this.status_ = status;
    this.pendingPickKey_ = '';
    this.lastError_ = null;
    this.maybeAutoPickDefault_();
    this.requestUpdate();
  }

  // Adapter from the unified `nexusCurrentSessionState` payload onto the
  // existing `status_` shape. We only care about `sessionId` + `model` —
  // the picker's other state (catalog, options, autoPick) is unchanged.
  // Snapshot replaces `status_`; delta only updates fields that were
  // actually present (model omitted when only counters/phase/active
  // changed).
  private onCurrentSessionState_(
      payload: NexusCurrentSessionWirePayload): void {
    if (!payload) return;
    // Track phase regardless of model — drives the thinking-state
    // border-runner animation on the trigger. Snapshot always carries
    // a phase; deltas only when phase changed.
    if (payload.kind === 'snapshot') {
      this.phase_ = payload.phase ?? 'idle';
      this.onStatusChanged_({
        sessionId: payload.sessionId,
        ...(payload.model ? {selection: payload.model} : {}),
      });
      return;
    }
    if ('phase' in payload && payload.phase) {
      this.phase_ = payload.phase;
      // Phase-only deltas still need a re-render to flip the
      // thinking-state class on the trigger; requestUpdate is cheap.
      this.requestUpdate();
    }
    // For status_ updates only react if `model` is actually present.
    if (!('model' in payload)) return;
    this.onStatusChanged_({
      sessionId: payload.sessionId,
      ...(payload.model ? {selection: payload.model} : {}),
    });
  }

  // If the active session's stored selection points at a provider/model
  // that's no longer verified or enabled, pick the first available
  // option and push it through. Covers post-reset (verified provider
  // gone) and stale-session (model removed from the catalog) cases.
  //
  // CRITICALLY: this does NOT auto-pick when `status_` is null (i.e.
  // before nexus_web has had a chance to publish the worker's resolved
  // selection). nexus_web ALWAYS publishes one initial status carrying
  // the persisted-pref default; pre-empting it with `options_[0]` would
  // override the user's `default_model` preference whenever the popup's
  // first catalog message lands before the worker's first status. The
  // worker used to silently reject those auto-picks (the catalog it
  // gated on didn't include the popup's curated entries), so the bug
  // was invisible — until that gate was widened.
  private maybeAutoPickDefault_(): void {
    if (this.options_.length === 0) return;
    if (this.pendingPickKey_) return;
    const sel = this.status_?.selection;
    if (!sel) return;
    const selectionIsValid = this.options_.some(
        o => o.providerId === sel.providerId && o.modelId === sel.modelId);
    if (selectionIsValid) return;
    const fallback = this.options_[0];
    if (!fallback) return;
    void this.sendPick_(fallback);
  }

  private onPickFailed_(payload: PickFailedPayload): void {
    this.pendingPickKey_ = '';
    this.lastError_ = payload?.error ?? 'Failed to switch model';
    this.requestUpdate();
  }

  // --- Outside-click + keyboard --------------------------------------

  private attachOutsideClickHandler_(): void {
    if (this.outsideClickHandler_) return;
    this.outsideClickHandler_ = (event: PointerEvent) => {
      // CANNOT use `event.target` here. We're a Lit element rendered
      // inside two layers of shadow DOM (`<nexus-page>` → `<nexus-topbar>`
      // → us). Retargeting walks `event.target` up to the *outermost*
      // host that's still inside the open shadow tree visible to the
      // window — which is `<nexus-page>`, an ANCESTOR of us. That makes
      // `this.contains(event.target)` return false for clicks that
      // genuinely landed inside our popup, so the popup would close
      // before the row/tier `@click` handlers fired.
      //
      // `composedPath()` returns the full pre-retargeting path (across
      // shadow boundaries). If `this` appears in it, the click was
      // inside our shadow tree. Same approach as the original React
      // picker, just shadow-DOM-aware.
      const path = event.composedPath();
      if (path.includes(this)) {
        return;
      }
      this.closePopup_();
    };
    window.addEventListener('pointerdown', this.outsideClickHandler_);
  }

  private detachOutsideClickHandler_(): void {
    if (!this.outsideClickHandler_) return;
    window.removeEventListener('pointerdown', this.outsideClickHandler_);
    this.outsideClickHandler_ = null;
  }

  private closePopup_() {
    this.open_ = false;
    this.detachOutsideClickHandler_();
  }

  private onTriggerClick_() {
    if (this.open_) {
      this.closePopup_();
      return;
    }
    this.open_ = true;
    this.query_ = '';
    this.lastError_ = null;
    this.attachOutsideClickHandler_();
  }

  private onSearchKeyDown_(event: KeyboardEvent) {
    if (event.key === 'Escape') {
      this.closePopup_();
    }
  }

  private onSearchInput_(event: Event) {
    this.query_ = (event.target as HTMLInputElement).value;
  }

  // --- Pick → chrome.send → C++ -------------------------------------

  private optionKey_(option: PickerOption, tier?: string): string {
    return tier ? `${option.providerId}|${option.modelId}|${tier}` :
                  `${option.providerId}|${option.modelId}`;
  }

  private async sendPick_(option: PickerOption, tier?: string): Promise<void> {
    const key = this.optionKey_(option, tier);
    this.pendingPickKey_ = key;
    this.lastError_ = null;

    const payload: SessionSelection = {
      providerId: option.providerId,
      modelId: option.modelId,
    };
    if (tier) payload.thinkingTier = tier;

    try {
      const raw: unknown =
          await sendWithPromise('pickNexusSelection', payload);
      const result = raw as PickNexusSelectionResult;
      if (!result || !result.ok) {
        this.pendingPickKey_ = '';
        this.lastError_ = result?.error ?? 'Failed to switch model';
        this.requestUpdate();
        return;
      }
      // C++ accepted the pick but no bridge-event pusher was registered
      // (the guest webview that owns the bridge adapter isn't online
      // yet). The global default is durable in prefs, but the active
      // session won't rebind. Surface this so the user knows to retry
      // once the chat surface finishes loading instead of waiting on a
      // status broadcast that's never coming.
      if (result.pusherInvoked === false) {
        this.pendingPickKey_ = '';
        this.lastError_ =
            'Chat panel is still starting — your pick was saved as the ' +
            'default, but please try again once the conversation loads.';
        this.requestUpdate();
        return;
      }
      // On success with the pusher invoked: keep pendingPickKey_ until
      // the next nexusSessionStatusChanged broadcast clears it.
    } catch (e) {
      this.pendingPickKey_ = '';
      this.lastError_ =
          e instanceof Error ? e.message : 'Failed to switch model';
      this.requestUpdate();
    }
  }

  private onRowClick_(option: PickerOption) {
    void this.sendPick_(option);
    this.closePopup_();
  }

  private onTierClick_(event: Event, option: PickerOption, tier: string) {
    event.stopPropagation();
    void this.sendPick_(option, tier);
    this.closePopup_();
  }

  private onOpenSettingsClick_() {
    this.closePopup_();
    // Two routes — whichever lands first wins:
    //   1. The global `customBrowserOpenNexusSettings(section)` hook that
    //      `<nexus-page>` registers on boot (nexus_page.ts:83). Works
    //      from anywhere on the page regardless of shadow-DOM nesting.
    //   2. The bubbling `nexus-open-settings` CustomEvent that the rest
    //      of the topbar already dispatches (nexus_topbar.ts:407).
    // Both end up calling `<nexus-page>::openSettings_('models')`.
    const host = window as unknown as {
      customBrowserOpenNexusSettings?: (section?: string) => void;
    };
    if (typeof host.customBrowserOpenNexusSettings === 'function') {
      host.customBrowserOpenNexusSettings('models');
      return;
    }
    this.dispatchEvent(new CustomEvent(
        'nexus-open-settings',
        {bubbles: true, composed: true, detail: {section: 'models'}}));
  }

  // --- Filtering -----------------------------------------------------

  private filteredOptions_(): PickerOption[] {
    const query = this.query_.trim().toLowerCase();
    if (!query) return this.options_;
    const terms = query.split(/\s+/).filter(Boolean);
    return this.options_.filter(option => {
      const haystack = [
        option.modelLabel,
        option.providerLabel,
        option.providerId,
        option.modelId,
      ].filter(Boolean).join(' ').toLowerCase();
      return terms.every(term => haystack.includes(term));
    });
  }

  private isProviderBoundary_(
      options: readonly PickerOption[], index: number): boolean {
    if (index <= 0) return false;
    return options[index - 1]!.providerId !== options[index]!.providerId;
  }

  // --- Selection lookup ----------------------------------------------

  private resolvedSelectedOption_(): PickerOption|null {
    const sel = this.status_?.selection;
    if (!sel?.modelId) return null;
    return this.options_.find(
               o => o.providerId === sel.providerId &&
                   o.modelId === sel.modelId) ?? null;
  }

  // --- Render --------------------------------------------------------

  override render() {
    const selectedOption = this.resolvedSelectedOption_();
    // Active thinking tier on the live selection, falling back to the
    // model's catalog default when the C++ broker doesn't carry one yet.
    const activeTier = this.status_?.selection?.thinkingTier ??
        selectedOption?.defaultThinkingTier;
    const showTier = !!activeTier && !!selectedOption?.availableThinkingTiers
        ?.length;
    const titleText = selectedOption ?
        (showTier ? `${selectedOption.modelLabel} · ${activeTier}` :
                    selectedOption.modelLabel) :
        'Select model';
    const triggerClasses =
        `qs-trigger${this.phase_ === 'thinking' ? ' qs-trigger--thinking' : ''}`;
    return html`
      <button class=${triggerClasses}
          @click=${this.onTriggerClick_}
          aria-haspopup="listbox"
          aria-expanded=${this.open_ ? 'true' : 'false'}
          title=${titleText}>
        ${this.renderIcon_(selectedOption)}
        <span class="qs-trigger__label">
          ${selectedOption?.modelLabel ??
              getString('customBrowserNexusQuickSettingsEmpty',
                        'Select model')}
          ${showTier && activeTier ? html`<span class="qs-trigger__label-tier"
              >&nbsp;·&nbsp;${activeTier}</span>` :
                        nothing}
        </span>
        <svg class="qs-trigger__caret" viewBox="0 0 12 12"
             aria-hidden="true">
          <path d="M3 5l3 3 3-3" fill="none" stroke="currentColor"
              stroke-width="1.5" stroke-linecap="round"
              stroke-linejoin="round"/>
        </svg>
      </button>
      ${this.open_ ? this.renderPopup_(selectedOption) : nothing}
    `;
  }

  private renderIcon_(option: PickerOption|null) {
    const url = iconDataUrl(option?.iconSvg);
    return html`
      <span class="qs-trigger__icon" aria-hidden="true">
        ${url ? html`<img src=${url} alt="">` :
                html`<span class="qs-trigger__icon-fallback">${
                    (option?.providerLabel ?? '?').charAt(0).toUpperCase()
                }</span>`}
      </span>
    `;
  }

  private renderRowIcon_(option: PickerOption) {
    const url = iconDataUrl(option.iconSvg);
    return html`
      <span class="qs-row__icon" aria-hidden="true">
        ${url ? html`<img src=${url} alt="">` :
                html`<span class="qs-row__icon-fallback">${
                    option.providerLabel.charAt(0).toUpperCase()
                }</span>`}
      </span>
    `;
  }

  private renderPopup_(selectedOption: PickerOption|null) {
    const filtered = this.filteredOptions_();
    const totalCount = this.options_.length;
    const placeholder = getString(
        'customBrowserNexusQuickSettingsSearchPlaceholder',
        `Search ${totalCount} models`);
    return html`
      <div class="qs-popup" role="dialog" aria-label="Quick settings">
        <div class="qs-search">
          <input type="text"
              .value=${this.query_}
              placeholder=${placeholder}
              @input=${this.onSearchInput_}
              @keydown=${this.onSearchKeyDown_}>
        </div>
        <div class="qs-list" role="listbox">
          ${filtered.length === 0 ? html`
            <div class="qs-empty">${
              totalCount === 0 ?
                  getString(
                      'customBrowserNexusQuickSettingsEmptyVerified',
                      'No verified providers yet — open settings to add one.') :
                  getString(
                      'customBrowserNexusQuickSettingsNoMatches',
                      'No models match your search.')}</div>
          ` : filtered.map((option, index) => this.renderRow_(
              option, index, filtered, selectedOption))}
        </div>
        ${this.lastError_ ? html`
          <div class="qs-error" role="alert">${this.lastError_}</div>
        ` : nothing}
        <div class="qs-footer">
          <button class="qs-footer__btn"
              @click=${this.onOpenSettingsClick_}>
            <svg class="qs-footer__icon" viewBox="0 0 16 16"
                 aria-hidden="true">
              <path
                d="M8 5.5a2.5 2.5 0 100 5 2.5 2.5 0 000-5zm6.6 3.4l-1.4-.7c.1-.4.1-.8 0-1.2l1.4-.7c.2-.1.3-.4.2-.6l-1-1.7c-.1-.2-.4-.3-.6-.2l-1.4.7c-.3-.3-.7-.5-1.1-.7l-.2-1.5c0-.3-.3-.5-.5-.5h-2c-.3 0-.5.2-.5.5l-.2 1.5c-.4.2-.8.4-1.1.7l-1.4-.7c-.2-.1-.5 0-.6.2l-1 1.7c-.1.2 0 .5.2.6l1.4.7c-.1.4-.1.8 0 1.2l-1.4.7c-.2.1-.3.4-.2.6l1 1.7c.1.2.4.3.6.2l1.4-.7c.3.3.7.5 1.1.7l.2 1.5c0 .3.3.5.5.5h2c.3 0 .5-.2.5-.5l.2-1.5c.4-.2.8-.4 1.1-.7l1.4.7c.2.1.5 0 .6-.2l1-1.7c.1-.2 0-.5-.2-.6z"
                fill="none" stroke="currentColor" stroke-width="1.2"
                stroke-linejoin="round"/>
            </svg>
            <span>${getString(
                'customBrowserNexusQuickSettingsOpenSettings',
                'Provider settings…')}</span>
          </button>
        </div>
      </div>
    `;
  }

  private renderRow_(
      option: PickerOption, index: number,
      filtered: readonly PickerOption[],
      selectedOption: PickerOption|null) {
    const isSelected = !!selectedOption &&
        selectedOption.providerId === option.providerId &&
        selectedOption.modelId === option.modelId;
    const rowKey = this.optionKey_(option);
    const isPending = this.pendingPickKey_.startsWith(rowKey);
    // For the active row, highlight the tier the user actually picked
    // (falling back to the catalog default when they clicked the row
    // body instead of a pill — that resets the selection to default).
    // For non-active rows, highlight the catalog default so users see
    // what would apply if they picked that row.
    const activeTierForRow = isSelected
        ? (this.status_?.selection?.thinkingTier
              ?? option.defaultThinkingTier)
        : option.defaultThinkingTier;
    return html`
      ${this.isProviderBoundary_(filtered, index) ?
          html`<div class="qs-divider" role="separator"></div>` : nothing}
      <button class="qs-row"
          role="option"
          aria-selected=${isSelected ? 'true' : 'false'}
          ?disabled=${isPending}
          @click=${() => this.onRowClick_(option)}>
        ${this.renderRowIcon_(option)}
        <span class="qs-row__text">
          <span class="qs-row__label">${option.modelLabel}</span>
          <span class="qs-row__caption">${option.providerLabel}</span>
        </span>
        ${this.renderTiers_(option, activeTierForRow)}
        ${isSelected ? html`
          <span class="qs-check" aria-hidden="true">
            <svg viewBox="0 0 12 12">
              <path d="M2.5 6.5l2.4 2.4L9.5 4.2" fill="none"
                  stroke="currentColor" stroke-width="2"
                  stroke-linecap="round" stroke-linejoin="round"/>
            </svg>
          </span>
        ` : nothing}
      </button>
    `;
  }

  private renderTiers_(option: PickerOption, activeTier: string|undefined) {
    const tiers = option.availableThinkingTiers ?? [];
    if (tiers.length === 0) return nothing;
    // Compact pill labels for the tier row. Defined as a method-local
    // const + arrow so the read sits in plain JS scope — Chromium's
    // WebUI preprocessed-tsc step reports TS6133 for module-level
    // helpers whose ONLY references live inside Lit `html\`\`` template
    // interpolations, so we keep the reads at statement level here.
    const tierLabels: Readonly<Record<string, string>> = {
      minimal: 'MIN',
      low: 'L',
      medium: 'M',
      high: 'H',
      'extra-high': 'X',
      max: 'MAX',
    };
    const labelFor = (tier: string): string =>
        tierLabels[tier] ?? tier.slice(0, 3).toUpperCase();
    const pills = tiers.map(tier => {
      const cls =
          tier === activeTier ? 'qs-tier qs-tier--active' : 'qs-tier';
      const label = labelFor(tier);
      return html`
        <span class=${cls}
            role="button"
            tabindex="0"
            @click=${(event: Event) => this.onTierClick_(event, option, tier)}
            @keydown=${(event: KeyboardEvent) =>
                this.onTierKeyDown_(event, option, tier)}
            title=${tier}>
          ${label}
        </span>
      `;
    });
    return html`<span class="qs-tiers">${pills}</span>`;
  }

  private onTierKeyDown_(event: KeyboardEvent, option: PickerOption,
                         tier: string) {
    if (event.key !== 'Enter' && event.key !== ' ') return;
    event.preventDefault();
    event.stopPropagation();
    void this.sendPick_(option, tier);
    this.closePopup_();
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'nexus-quick-settings': NexusQuickSettingsElement;
  }
}

customElements.define(
    NexusQuickSettingsElement.is, NexusQuickSettingsElement);
