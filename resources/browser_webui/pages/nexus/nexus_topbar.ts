import 'chrome://resources/cr_elements/cr_icon_button/cr_icon_button.js';
import 'chrome://resources/cr_elements/icons.html.js';
import {sendWithPromise} from 'chrome://resources/js/cr.js';
import {
  CrLitElement,
  css,
  html,
  nothing,
  type CSSResultGroup,
} from 'chrome://resources/lit/v3_0/lit.rollup.js';

import '../../components/native/native_view_aliases.js';
import {
  getProviderDisplayName,
  loadModelHistory,
  pushModelHistory,
  type ModelHistoryEntry,
  type ModelInfo,
  type ProviderInfo,
  type ProviderResult,
} from './nexus_shared.js';

// ----------------------------------------------------------------
// Topbar mode enum
// ----------------------------------------------------------------

export enum NexusTopbarMode {
  TITLE = 'title',
  NEXUS_VIEW = 'nexus_view',
}

export interface NexusTopbarTitleConfig {
  closeLabel?: string;
  icon?: string;  // emoji or short text for leading position
  showCloseButton?: boolean;
  title: string;
}

// ----------------------------------------------------------------
// Component
// ----------------------------------------------------------------

export class NexusTopbar extends CrLitElement {
  static override styles: CSSResultGroup = css`
    :host {
      --topbar-text-color: var(--cr-primary-text-color, var(--text-primary));
      --topbar-surface: color-mix(
          in srgb,
          var(--app-bg, #ffffff) 82%,
          var(--topbar-text-color) 18%);
      --topbar-surface-hover: color-mix(
          in srgb,
          var(--app-bg, #ffffff) 74%,
          var(--topbar-text-color) 26%);
      --topbar-surface-soft: color-mix(
          in srgb,
          var(--app-bg, #ffffff) 88%,
          var(--topbar-text-color) 12%);
      --topbar-outline:
          color-mix(in srgb, var(--topbar-text-color) 14%, transparent);
      --topbar-shadow:
          0 8px 20px color-mix(in srgb, var(--topbar-text-color) 10%, transparent);
      align-items: center;
      display: flex;
      gap: 8px;
      min-height: 44px;
      position: relative;
      width: 100%;
    }

    /* --- Shared cluster ----------------------------------------- */

    .cluster {
      align-items: center;
      display: flex;
      flex: 1 1 auto;
      min-height: 40px;
      min-width: 0;
      padding: 4px;
      position: relative;
      width: 100%;
    }

    .caption {
      flex: 0 0 auto;
      justify-self: end;
    }

    /* --- TITLE mode --------------------------------------------- */

    .title-bar {
      align-items: center;
      display: flex;
      inset: 0;
      justify-content: center;
      pointer-events: none;
      position: absolute;
      padding: 0 8px;
    }

    .title-shell {
      align-items: center;
      display: flex;
      gap: 8px;
      max-width: min(100%, 560px);
      min-width: 0;
      pointer-events: auto;
      -webkit-app-region: no-drag;
    }

    .title-pill {
      align-items: center;
      background: linear-gradient(
          135deg, var(--topbar-surface), var(--topbar-surface-soft));
      border: 1px solid var(--topbar-outline);
      border-radius: 999px;
      box-shadow: var(--topbar-shadow);
      color: var(--topbar-text-color);
      display: inline-flex;
      flex: 1 1 auto;
      gap: 8px;
      max-width: min(100%, 460px);
      min-height: 34px;
      min-width: 0;
      overflow: hidden;
      padding: 0 14px;
      white-space: nowrap;
    }

    .title-icon {
      display: inline-flex;
      flex: none;
      font-size: 1.1rem;
      line-height: 1;
    }

    .title-text {
      font-size: 0.92rem;
      font-weight: 600;
      overflow: hidden;
      text-overflow: ellipsis;
    }

    /* --- NEXUS_VIEW mode ---------------------------------------- */

    .nexus-view {
      align-items: center;
      display: flex;
      gap: 10px;
      min-width: 0;
      width: 100%;
      -webkit-app-region: no-drag;
    }

    .selector-shell {
      flex: 0 1 360px;
      max-width: 100%;
      min-width: 0;
      position: relative;
    }

    .selector-card {
      align-items: center;
      background: linear-gradient(
          135deg, var(--topbar-surface), var(--topbar-surface-soft));
      border: 1px solid var(--topbar-outline);
      border-radius: 15px;
      box-shadow: var(--topbar-shadow);
      color: var(--topbar-text-color);
      cursor: pointer;
      display: flex;
      gap: 12px;
      min-width: 0;
      padding: 6px 12px 6px 8px;
      transition:
          background 0.18s ease,
          border-color 0.18s ease,
          box-shadow 0.18s ease,
          transform 0.18s ease;
      width: 100%;
    }

    .selector-card:hover:not([disabled]),
    .selector-card:focus-visible:not([disabled]) {
      background: linear-gradient(
          135deg, var(--topbar-surface-hover), var(--topbar-surface));
      border-color:
          color-mix(in srgb, var(--topbar-text-color) 22%, transparent);
      box-shadow:
          0 12px 24px color-mix(in srgb, var(--topbar-text-color) 12%, transparent);
      outline: none;
      transform: translateY(-1px);
    }

    .selector-card[disabled] {
      box-shadow: none;
      cursor: default;
      opacity: 0.72;
    }

    .provider-badge {
      align-items: center;
      background: color-mix(
          in srgb,
          var(--app-bg, #ffffff) 72%,
          var(--topbar-text-color) 28%);
      border-radius: 12px;
      box-shadow: inset 0 0 0 1px
          color-mix(in srgb, var(--topbar-text-color) 10%, transparent);
      display: flex;
      flex: none;
      height: 34px;
      justify-content: center;
      width: 34px;
    }

    .provider-logo {
      height: 20px;
      object-fit: contain;
      width: 20px;
    }

    .selector-copy {
      align-items: flex-start;
      display: flex;
      flex: 1 1 auto;
      flex-direction: column;
      gap: 2px;
      min-width: 0;
    }

    .selector-provider {
      color: color-mix(in srgb, var(--topbar-text-color) 60%, transparent);
      font-size: 10px;
      font-weight: 700;
      letter-spacing: 0.08em;
      line-height: 1.1;
      max-width: 100%;
      overflow: hidden;
      text-overflow: ellipsis;
      text-transform: uppercase;
      white-space: nowrap;
    }

    .selector-model {
      font-size: 13px;
      font-weight: 600;
      line-height: 1.2;
      max-width: 100%;
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
    }

    .selector-chevron {
      border-block-end: 1.6px solid currentColor;
      border-inline-end: 1.6px solid currentColor;
      flex: none;
      height: 9px;
      margin-inline-start: 4px;
      opacity: 0.72;
      transform: rotate(45deg) translateY(-1px);
      width: 9px;
    }

    .settings-button,
    .title-close-button {
      --cr-icon-button-fill-color: var(--topbar-text-color);
      --cr-icon-button-hover-background-color: transparent;
      --cr-icon-button-icon-size: 16px;
      --cr-icon-button-size: 34px;
      background: linear-gradient(
          135deg, var(--topbar-surface-soft), var(--topbar-surface));
      border: 1px solid var(--topbar-outline);
      border-radius: 12px;
      box-shadow: var(--topbar-shadow);
      flex: none;
      transition:
          background 0.18s ease,
          border-color 0.18s ease,
          box-shadow 0.18s ease,
          transform 0.18s ease;
      -webkit-app-region: no-drag;
    }

    .settings-button:hover,
    .settings-button:focus-visible,
    .title-close-button:hover,
    .title-close-button:focus-visible {
      background: linear-gradient(
          135deg, var(--topbar-surface-hover), var(--topbar-surface));
      border-color:
          color-mix(in srgb, var(--topbar-text-color) 22%, transparent);
      box-shadow:
          0 12px 24px color-mix(in srgb, var(--topbar-text-color) 12%, transparent);
      outline: none;
      transform: translateY(-1px);
    }

    .picker-proxy-select {
      -webkit-appearance: none;
      appearance: none;
      background: transparent;
      border: 0;
      inset: 0;
      margin: 0;
      opacity: 0;
      pointer-events: none;
      position: absolute;
    }
  `;

  // --- State -----------------------------------------------------

  private mode_: NexusTopbarMode = NexusTopbarMode.NEXUS_VIEW;
  private titleConfig_: NexusTopbarTitleConfig = {title: ''};

  private activeProviderId_ = '';
  private providers_: ProviderInfo[] = [];
  private historyEntries_: ModelHistoryEntry[] = [];

  // --- Lifecycle -------------------------------------------------

  override firstUpdated() {
    void this.loadProviders_();
    this.historyEntries_ = loadModelHistory();
  }

  // --- Public API ------------------------------------------------

  public setMode(mode: NexusTopbarMode, config?: NexusTopbarTitleConfig) {
    this.mode_ = mode;
    if (config) {
      this.titleConfig_ = config;
    }
    this.requestUpdate();
  }

  public refresh() {
    void this.loadProviders_();
    this.historyEntries_ = loadModelHistory();
    this.requestUpdate();
  }

  // --- Data loading ----------------------------------------------

  private async loadProviders_() {
    try {
      const result = await sendWithPromise('getAgentProviders') as ProviderResult;
      this.providers_ = Array.isArray(result.providers) ? result.providers : [];
      this.activeProviderId_ = result.activeProviderId || '';
      if (!this.activeProviderId_ && this.providers_.length > 0) {
        this.activeProviderId_ = this.providers_[0]!.id;
      }
      this.requestUpdate();
    } catch (e) {
      console.error('Failed to load providers in topbar:', e);
    }
  }

  private getCurrentProvider_(): ProviderInfo | null {
    return this.providers_.find(p => p.id === this.activeProviderId_) || null;
  }

  private getCurrentModelId_(): string {
    const p = this.getCurrentProvider_();
    if (!p) {
      return '';
    }
    return p.defaultModel || p.model || '';
  }

  private normalizeModelLookupKey_(value: string): string {
    const normalized = value.trim().toLowerCase();
    return normalized.startsWith('models/') ? normalized.slice(7) : normalized;
  }

  private getCurrentModelInfo_(): ModelInfo | null {
    const provider = this.getCurrentProvider_();
    const modelId = this.getCurrentModelId_();
    if (!provider || !modelId) {
      return null;
    }

    const normalizedModelId = this.normalizeModelLookupKey_(modelId);
    return provider.models.find(model => {
             return this.normalizeModelLookupKey_(model.id) ===
                     normalizedModelId ||
                 this.normalizeModelLookupKey_(model.name) ===
                     normalizedModelId;
           }) ||
        null;
  }

  private getCurrentHistoryModelName_(): string {
    const provider = this.getCurrentProvider_();
    const modelId = this.getCurrentModelId_();
    if (!provider || !modelId) {
      return '';
    }

    const normalizedModelId = this.normalizeModelLookupKey_(modelId);
    const historyEntry = this.historyEntries_.find(entry => {
      return entry.providerId === provider.id &&
          this.normalizeModelLookupKey_(entry.modelId) === normalizedModelId;
    });
    return historyEntry?.modelName || '';
  }

  private getCurrentModelName_(): string {
    const modelId = this.getCurrentModelId_();
    if (!modelId) {
      return '';
    }

    return this.getCurrentModelInfo_()?.name ||
        this.getCurrentHistoryModelName_() || modelId;
  }

  private getTooltip_(): string {
    const p = this.getCurrentProvider_();
    if (!p) {
      return '';
    }
    return `Provider: ${getProviderDisplayName(p)}\nModel: ${this.getCurrentModelName_()}`;
  }

  private getCurrentSelectionEntry_(): ModelHistoryEntry | null {
    const provider = this.getCurrentProvider_();
    const modelId = this.getCurrentModelId_();
    if (!provider || !modelId) {
      return null;
    }

    return {
      modelId,
      modelName: this.getCurrentModelName_(),
      providerId: provider.id,
      providerName: getProviderDisplayName(provider),
    };
  }

  private getHistoryEntryKey_(
      entry: Pick<ModelHistoryEntry, 'modelId'|'providerId'>): string {
    return `${entry.providerId}\u0000${entry.modelId}`;
  }

  private getHistoryEntryValue_(
      entry: Pick<ModelHistoryEntry, 'modelId'|'providerId'>): string {
    return JSON.stringify([entry.providerId, entry.modelId]);
  }

  private getHistoryEntriesForSelect_(): ModelHistoryEntry[] {
    const currentEntry = this.getCurrentSelectionEntry_();
    const entries = [...this.historyEntries_];
    if (!currentEntry) {
      return entries;
    }

    const currentKey = this.getHistoryEntryKey_(currentEntry);
    if (entries.some(entry => this.getHistoryEntryKey_(entry) === currentKey)) {
      return entries;
    }

    return [currentEntry, ...entries];
  }

  private getHistoryPicker_(): HTMLSelectElement | null {
    return this.shadowRoot?.querySelector<HTMLSelectElement>(
               '.picker-proxy-select') ||
        null;
  }

  // --- Event handlers --------------------------------------------

  private onSettingsClick_() {
    this.dispatchEvent(new CustomEvent(
        'nexus-open-settings', {bubbles: true, composed: true}));
  }

  private onTitleCloseClick_() {
    this.dispatchEvent(new CustomEvent(
        'nexus-settings-close', {bubbles: true, composed: true}));
  }

  private onSelectorClick_() {
    this.openHistoryPicker_();
  }

  private onSelectorKeyDown_(event: KeyboardEvent) {
    if (event.key !== 'ArrowDown' && event.key !== 'ArrowUp') {
      return;
    }

    event.preventDefault();
    this.openHistoryPicker_();
  }

  private openHistoryPicker_() {
    const select = this.getHistoryPicker_();
    if (!select || select.disabled) {
      return;
    }

    try {
      select.showPicker();
    } catch (error) {
      try {
        select.focus({preventScroll: true});
        select.click();
      } catch (fallbackError) {
        console.error('Failed to open model picker:', error, fallbackError);
      }
    }
  }

  private onHistorySelectChange_(event: Event) {
    const select = event.target as HTMLSelectElement;
    const entry = this.getHistoryEntriesForSelect_().find(
        item => this.getHistoryEntryValue_(item) === select.value);
    if (!entry) {
      return;
    }

    if (entry.providerId === this.activeProviderId_ &&
        entry.modelId === this.getCurrentModelId_()) {
      return;
    }

    void this.selectHistoryEntry_(entry);
  }

  private async selectHistoryEntry_(entry: ModelHistoryEntry) {
    try {
      // Switch provider if needed.
      if (entry.providerId !== this.activeProviderId_) {
        await sendWithPromise('setActiveProvider', entry.providerId);
        this.activeProviderId_ = entry.providerId;
      }
      // Set model.
      await sendWithPromise(
          'updateProviderConfig', entry.providerId,
          {default_model: entry.modelId});
      const p = this.getCurrentProvider_();
      if (p) {
        p.defaultModel = entry.modelId;
      }
      // Update history.
      this.historyEntries_ = pushModelHistory(entry);
      this.requestUpdate();
    } catch (err) {
      console.error('Failed to switch model via history:', err);
      this.requestUpdate();
    }
  }

  // --- Render ----------------------------------------------------

  override render() {
    return html`
      <div class="cluster">
        ${this.mode_ === NexusTopbarMode.TITLE ? this.renderTitle_() :
                                                this.renderNexusView_()}
      </div>
      <caption-browser-button align-x="right"></caption-browser-button>
      <caption-button-container class="caption" align-x="right">
      </caption-button-container>
    `;
  }

  private renderTitle_() {
    return html`
      <div class="title-bar">
        <div class="title-shell">
          <div class="title-pill">
            ${this.titleConfig_.icon
                ? html`<span class="title-icon">${this.titleConfig_.icon}</span>`
                : nothing}
            <span class="title-text">${this.titleConfig_.title}</span>
          </div>
          ${this.titleConfig_.showCloseButton ? html`
            <cr-icon-button class="title-close-button"
                iron-icon="cr:close"
                aria-label=${this.titleConfig_.closeLabel || 'Close settings'}
                title=${this.titleConfig_.closeLabel || 'Close settings'}
                @click=${this.onTitleCloseClick_}>
            </cr-icon-button>
          ` : nothing}
        </div>
      </div>
    `;
  }

  private renderNexusView_() {
    const hasProvider = this.activeProviderId_ !== '';
    if (!hasProvider) {
      return nothing;
    }

    const currentProvider = this.getCurrentProvider_();
    if (!currentProvider) {
      return nothing;
    }

    const currentEntry = this.getCurrentSelectionEntry_();
    const entries = this.getHistoryEntriesForSelect_();
    const selectedValue = currentEntry ?
        this.getHistoryEntryValue_(currentEntry) :
        (entries[0] ? this.getHistoryEntryValue_(entries[0]) : '');
    const providerName = getProviderDisplayName(currentProvider);
    const currentModelName = this.getCurrentModelName_();

    return html`
      <div class="nexus-view">
        <div class="selector-shell">
          <button class="selector-card"
              type="button"
              @click=${this.onSelectorClick_}
              @keydown=${this.onSelectorKeyDown_}
              aria-label=${entries.length > 1 ?
                  `Switch model. Current selection: ${providerName}, ${currentModelName}` :
                  `Current model: ${providerName}, ${currentModelName}`}
              aria-haspopup="listbox"
              title=${this.getTooltip_()}
              ?disabled=${entries.length <= 1}>
            <div class="provider-badge">
              <img src="/components/nexus/${this.activeProviderId_}.svg"
                  class="provider-logo" alt="">
            </div>
            <div class="selector-copy">
              <span class="selector-provider">${providerName}</span>
              <span class="selector-model">${currentModelName}</span>
            </div>
            <div class="selector-chevron"></div>
          </button>
          <select class="picker-proxy-select"
              aria-label="Switch model"
              tabindex="-1"
              .value=${selectedValue}
              ?disabled=${entries.length <= 1}
              @change=${this.onHistorySelectChange_}>
            ${entries.map(entry => html`
              <option value=${this.getHistoryEntryValue_(entry)}>
                ${entry.providerName} · ${entry.modelName}
              </option>
            `)}
          </select>
        </div>
        <cr-icon-button class="settings-button"
            iron-icon="cr:settings_icon"
            @click=${this.onSettingsClick_}
            aria-label="Open settings"
            title="Settings">
        </cr-icon-button>
      </div>
    `;
  }
}

customElements.define('nexus-topbar', NexusTopbar);
