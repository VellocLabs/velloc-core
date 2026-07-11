import 'chrome://resources/cr_elements/cr_button/cr_button.js';

import {sendWithPromise} from 'chrome://resources/js/cr.js';
import {
  CrLitElement,
  css,
  html,
  nothing,
  type CSSResultGroup,
} from 'chrome://resources/lit/v3_0/lit.rollup.js';

import {
  DEFAULT_BASE_URLS,
  getBaseUrlValidationMessage,
  getProviderDisplayName,
  getString,
  getStringF,
  hasDefaultBaseUrl,
  type ModelInfo,
  paneStyles,
  type ProviderConfigUpdates,
  type ProviderInfo,
  type ProviderResult,
  type ResetProviderConfigResult,
  type SettingsStatus,
  type StatusTone,
  type UpdateProviderConfigResult,
  type VerificationMetadata,
  type VerifyProviderConfigResult,
} from './nexus_shared.js';
import './nexus_subpage_layout.js';

const AUTO_VERIFY_DEBOUNCE_MS = 500;

export class NexusSettingsModelsElement extends CrLitElement {
  static get is() {
    return 'nexus-settings-models';
  }

  static override styles: CSSResultGroup = [
    paneStyles,
    css`
      .loading-text {
        color: var(--cr-secondary-text-color, var(--text-secondary));
        line-height: 20px;
        margin: 0;
      }

      .provider-option-icon {
        height: 20px;
        width: 20px;
        flex: none;
      }

      .custom-select {
        position: relative;
        user-select: none;
      }

      .custom-select-button {
        background: var(
            --md-background-color,
            var(--cr-card-background-color, var(--app-surface)));
        border: 1px solid var(--cr-separator-color);
        border-radius: 12px;
        box-sizing: border-box;
        color: var(--cr-primary-text-color, var(--text-primary));
        font: inherit;
        min-height: 48px;
        padding: 12px 14px;
        width: 100%;
        display: flex;
        align-items: center;
        gap: 8px;
        cursor: pointer;
        text-align: left;
      }

      .custom-select-button:focus {
        border-color: var(--cr-focus-outline-color);
        box-shadow: 0 0 0 1px var(--cr-focus-outline-color);
        outline: none;
      }

      .custom-select-button .chevron {
        margin-left: auto;
        width: 0;
        height: 0;
        border-left: 5px solid transparent;
        border-right: 5px solid transparent;
        border-top: 5px solid currentColor;
      }

      .dropdown-backdrop {
        position: fixed;
        inset: 0;
        z-index: 99;
      }

      .custom-select-dropdown {
        position: absolute;
        top: calc(100% + 4px);
        left: 0;
        width: 100%;
        background: var(--cr-card-background-color, var(--app-surface));
        border: 1px solid var(--cr-separator-color);
        border-radius: 12px;
        box-shadow: 0 4px 16px rgba(0, 0, 0, 0.18);
        z-index: 100;
        max-height: 260px;
        overflow-y: auto;
        display: flex;
        flex-direction: column;
        padding: 4px 0;
      }

      .custom-option {
        display: flex;
        align-items: center;
        gap: 10px;
        padding: 12px 14px;
        cursor: pointer;
        transition: background-color 0.12s;
      }

      .custom-option:hover {
        background: var(--cr-hover-background-color, rgba(0,0,0,0.06));
      }

      .custom-option.selected {
        background: var(--cr-active-background-color, rgba(0,0,0,0.08));
        font-weight: 500;
      }

      .sr-live {
        clip: rect(0 0 0 0);
        clip-path: inset(50%);
        height: 1px;
        overflow: hidden;
        position: absolute;
        white-space: nowrap;
        width: 1px;
      }

      .dialog-backdrop {
        align-items: center;
        background: rgba(0, 0, 0, 0.32);
        inset: 0;
        display: flex;
        justify-content: center;
        padding: 16px;
        position: fixed;
        z-index: 120;
      }

      .dialog-card {
        background: var(--cr-card-background-color, var(--app-surface));
        border: 1px solid var(--cr-separator-color);
        border-radius: 16px;
        box-shadow: 0 16px 40px rgba(0, 0, 0, 0.22);
        box-sizing: border-box;
        max-width: 460px;
        padding: 20px;
        width: min(460px, calc(100vw - 32px));
      }

      .dialog-title {
        color: var(--cr-primary-text-color, var(--text-primary));
        font-size: 18px;
        font-weight: 500;
        line-height: 24px;
        margin: 0 0 8px;
      }

      .dialog-body {
        color: var(--cr-secondary-text-color, var(--text-secondary));
        line-height: 20px;
        margin: 0;
        white-space: pre-line;
      }

      .dialog-actions {
        display: flex;
        gap: 8px;
        justify-content: flex-end;
        margin-top: 20px;
      }
    `,
  ];

  // --- State -------------------------------------------------------

  private apiKey_ = '';
  private baseUrl_ = '';
  private debounceTimer_ = 0;
  private errorCode_ = '';
  private errorMessage_ = '';
  private hasStoredApiKey_ = false;
  private hasPendingProviderChanges_ = false;
  private liveAnnouncement_ = '';
  private loading_ = true;
  private modelOptions_: ModelInfo[] = [];
  private providerActionError_ = '';
  private providers_: ProviderInfo[] = [];
  private providerDropdownOpen_ = false;
  private resetDialogOpen_ = false;
  private resetting_ = false;
  private selectedProviderId_ = '';
  private statusMode_: SettingsStatus = 'idle';
  private verifyToken_ = 0;

  // --- Lifecycle ---------------------------------------------------

  override firstUpdated() {
    void this.loadProviders_();
  }

  override disconnectedCallback() {
    super.disconnectedCallback();
    if (this.debounceTimer_) {
      clearTimeout(this.debounceTimer_);
      this.debounceTimer_ = 0;
    }
    this.verifyToken_++;
  }

  // --- Helpers -----------------------------------------------------

  private getCurrentProvider_(): ProviderInfo|null {
    return this.providers_.find(
               provider => provider.id === this.selectedProviderId_) ||
        null;
  }

  private supportsBaseUrl_(provider: ProviderInfo): boolean {
    return hasDefaultBaseUrl(provider.id);
  }

  private requiresApiKey_(provider: ProviderInfo): boolean {
    return provider.type === 'cloud';
  }

  private getProviderNote_(provider: ProviderInfo): string {
    if (provider.id === 'ollama') {
      return getString('customBrowserNexusOllamaNote');
    }
    return '';
  }

  private setStatusError_(message: string, errorCode = '') {
    this.errorCode_ = errorCode;
    this.errorMessage_ = message;
    this.statusMode_ = 'error';
    this.announce_(message);
  }

  private announce_(text: string) {
    this.liveAnnouncement_ = '';
    this.requestUpdate();
    // Allow the DOM to clear before setting new text so screen readers
    // re-announce even if the text is identical.
    requestAnimationFrame(() => {
      this.liveAnnouncement_ = text;
      this.requestUpdate();
    });
  }

  private mapErrorCodeToMessage_(errorCode: string): string {
    switch (errorCode) {
      case 'auth_unauthorized':
        return getString('customBrowserNexusErrorUnauthorized');
      case 'auth_forbidden':
        return getString('customBrowserNexusErrorForbidden');
      case 'endpoint_invalid_url':
        return getString('customBrowserNexusErrorInvalidBaseUrl');
      case 'endpoint_unreachable':
        return getString('customBrowserNexusErrorEndpointUnreachable');
      case 'endpoint_timeout':
        return getString('customBrowserNexusErrorTimeout');
      case 'discovery_no_models':
        return getString('customBrowserNexusErrorNoModels');
      default:
        return this.errorMessage_ ||
            getString('customBrowserNexusErrorUnexpectedResponse');
    }
  }

  private getStatusPresentation_():
      {chip: string, detail: string, title: string, tone: StatusTone} {
    const provider = this.getCurrentProvider_();

    switch (this.statusMode_) {
      case 'verifying':
        return {
          chip: getString('customBrowserNexusStatusChipConnecting'),
          detail: getString('customBrowserNexusStatusFetchingModels'),
          title: getString('customBrowserNexusStatusConnecting'),
          tone: 'info',
        };
      case 'ready': {
        const modelCount = this.modelOptions_.length;
        return {
          chip: getString('customBrowserNexusStatusChipConnected'),
          detail: modelCount > 0 ?
              (modelCount === 1 ?
                   getStringF(
                       'customBrowserNexusStatusModelsFoundSingular',
                       '$1 model found', modelCount) :
                   getStringF(
                       'customBrowserNexusStatusModelsFoundPlural',
                       '$1 models found', modelCount)) :
              getString('customBrowserNexusProviderReady'),
          title: getString('customBrowserNexusStatusConnected'),
          tone: 'success',
        };
      }
      case 'draft':
        return {
          chip: getString('customBrowserNexusStatusChipChangesPending'),
          detail: getString('customBrowserNexusStatusConfigChanged'),
          title: getString('customBrowserNexusStatusVerificationRequired'),
          tone: 'warning',
        };
      case 'error':
        return {
          chip: getString('customBrowserNexusStatusChipFailed'),
          detail: this.mapErrorCodeToMessage_(this.errorCode_),
          title: getString('customBrowserNexusStatusConnectionFailed'),
          tone: 'error',
        };
      default:
        return {
          chip: getString('customBrowserNexusStatusChipNotConfigured'),
          detail: provider ?
              getString('customBrowserNexusProviderSetupRequired') :
              getString('customBrowserNexusStatusSelectProvider'),
          title: getString('customBrowserNexusStatusNotConfigured'),
          tone: 'warning',
        };
    }
  }

  // --- Capability checks -------------------------------------------

  private canResetProvider_(): boolean {
    return !this.loading_ && !this.resetting_ && this.statusMode_ === 'ready';
  }

  private updateProviderRecord_(
      providerId: string,
      updates: Partial<ProviderInfo>) {
    this.providers_ = this.providers_.map(provider => {
      if (provider.id !== providerId) {
        return provider;
      }

      return {
        ...provider,
        ...updates,
      };
    });
  }

  private buildConnectionUpdates_(
      provider: ProviderInfo,
      lastVerification: VerificationMetadata|null,
      overrides?: {apiKey?: string, baseUrl?: string}): ProviderConfigUpdates {
    const updates: ProviderConfigUpdates = {};
    const baseUrl = overrides?.baseUrl ?? this.baseUrl_.trim();
    const apiKey = overrides?.apiKey ?? this.apiKey_.trim();

    if (this.supportsBaseUrl_(provider)) {
      updates.base_url = baseUrl;
    }
    if (this.requiresApiKey_(provider) && apiKey) {
      updates.api_key = apiKey;
    }
    if (lastVerification) {
      updates.last_verification = lastVerification;
    }

    return updates;
  }

  private async persistVerifiedProvider_(
      provider: ProviderInfo,
      updates: ProviderConfigUpdates): Promise<boolean> {
    try {
      const saveResult = await sendWithPromise(
                             'updateProviderConfig', provider.id,
                             updates) as UpdateProviderConfigResult;
      if (!saveResult.success) {
        this.providerActionError_ =
            getString('customBrowserNexusUpdateFailed');
        return false;
      }
      this.hasPendingProviderChanges_ = false;
      if (this.requiresApiKey_(provider) &&
          typeof updates.api_key === 'string' && updates.api_key.length > 0) {
        this.hasStoredApiKey_ = true;
      }

      this.updateProviderRecord_(provider.id, {
        baseUrl: updates.base_url || provider.baseUrl,
        hasStoredApiKey: this.requiresApiKey_(provider) ?
            (this.hasStoredApiKey_ || !!updates.api_key) :
            provider.hasStoredApiKey,
        isConfigured: true,
        lastVerification: updates.last_verification,
        models: [...this.modelOptions_],
      });
      return true;
    } catch (error) {
      console.error('Failed to store Nexus provider settings:', error);
      this.providerActionError_ = getString('customBrowserNexusUpdateFailed');
      return false;
    }
  }

  private getResetDialogTitle_(provider: ProviderInfo): string {
    return getStringF(
        'customBrowserNexusResetDialogTitle',
        'Reset $1 connection?', getProviderDisplayName(provider));
  }

  private getResetDialogBody_(provider: ProviderInfo): string {
    return getStringF(
        'customBrowserNexusResetDialogBody',
        'This clears the saved connection details for $1, removes the ' +
            'verified state, and restores the default values for this ' +
            'provider.',
        getProviderDisplayName(provider));
  }

  // --- Data loading ------------------------------------------------

  private applyProviderSelection_(provider: ProviderInfo) {
    this.verifyToken_++;
    this.selectedProviderId_ = provider.id;
    this.baseUrl_ = this.supportsBaseUrl_(provider) ?
        (provider.baseUrl || DEFAULT_BASE_URLS[provider.id] || '') :
        '';
    this.apiKey_ = '';
    this.hasStoredApiKey_ = !!provider.hasStoredApiKey;
    this.modelOptions_ = provider.isConfigured ? [...provider.models] : [];
    this.hasPendingProviderChanges_ = false;
    this.errorCode_ = '';
    this.errorMessage_ = '';
    this.providerActionError_ = '';
    this.resetDialogOpen_ = false;
    this.statusMode_ = provider.isConfigured ? 'ready' : 'idle';
    this.requestUpdate();
  }

  private async loadProviders_() {
    this.loading_ = true;
    this.requestUpdate();

    try {
      const result =
          await sendWithPromise('getAgentProviders') as ProviderResult;
      this.providers_ =
          Array.isArray(result.providers) ? result.providers : [];

      if (!this.providers_.length) {
        this.selectedProviderId_ = '';
        this.modelOptions_ = [];
        this.hasPendingProviderChanges_ = false;
        this.statusMode_ = 'idle';
        return;
      }

      let nextProviderId = this.selectedProviderId_;
      if (!this.providers_.some(
              provider => provider.id === nextProviderId)) {
        nextProviderId =
            result.activeProviderId ||
            this.providers_.find(provider => provider.isConfigured)?.id ||
            this.providers_[0]!.id;
      }

      const provider =
          this.providers_.find(item => item.id === nextProviderId) ||
          this.providers_[0]!;
      this.applyProviderSelection_(provider);
    } catch (error) {
      console.error('Failed to load Nexus providers:', error);
      this.providers_ = [];
      this.selectedProviderId_ = '';
      this.modelOptions_ = [];
      this.hasPendingProviderChanges_ = false;
      this.setStatusError_(
          getString('customBrowserNexusErrorUnexpectedResponse'));
    } finally {
      this.loading_ = false;
      this.requestUpdate();
    }
  }

  // --- Auto-verify -------------------------------------------------

  private scheduleAutoVerify_() {
    if (this.debounceTimer_) {
      clearTimeout(this.debounceTimer_);
    }
    this.debounceTimer_ = window.setTimeout(() => {
      this.debounceTimer_ = 0;
      void this.autoVerify_();
    }, AUTO_VERIFY_DEBOUNCE_MS);
  }

  private async autoVerify_() {
    const provider = this.getCurrentProvider_();
    if (!provider || this.loading_ || this.resetting_ ||
        this.statusMode_ === 'verifying') {
      return;
    }

    if (!this.validateConnectionInputs_()) {
      this.requestUpdate();
      return;
    }

    const verifyToken = ++this.verifyToken_;
    this.errorCode_ = '';
    this.errorMessage_ = '';
    this.providerActionError_ = '';
    this.statusMode_ = 'verifying';
    this.announce_(getString(
        'customBrowserNexusStatusConnecting', 'Connecting…'));
    this.requestUpdate();

    const baseUrl = this.baseUrl_.trim();
    const apiKey = this.apiKey_.trim();
    const updates = this.buildConnectionUpdates_(
        provider, null, {apiKey, baseUrl});

    try {
      const result = await sendWithPromise(
                         'verifyProviderConfig', provider.id,
                         updates) as VerifyProviderConfigResult;

      if (verifyToken !== this.verifyToken_) return;

      if (!result.success) {
        this.setStatusError_(
            this.mapErrorCodeToMessage_(result.errorCode || ''),
            result.errorCode || '');
        this.modelOptions_ = [];
        this.requestUpdate();
        return;
      }

      const models = Array.isArray(result.models) ? result.models : [];
      if (!models.length) {
        this.setStatusError_(
            getString('customBrowserNexusErrorNoModels'),
            'discovery_no_models');
        this.modelOptions_ = [];
        this.requestUpdate();
        return;
      }

      this.modelOptions_ = models;
      this.statusMode_ = 'ready';
      const lastVerificationMetadata: VerificationMetadata = {
        baseUrl: result.baseUrl || baseUrl,
        modelCount: result.modelCount || models.length,
        providerId: provider.id,
        verifiedAtMs: result.verifiedAtMs,
      };
      if (this.hasPendingProviderChanges_) {
        const persisted = await this.persistVerifiedProvider_(
            provider,
            this.buildConnectionUpdates_(
                provider, lastVerificationMetadata, {apiKey, baseUrl}));
        if (verifyToken !== this.verifyToken_) return;
        if (!persisted) {
          this.requestUpdate();
          return;
        }
      }
      this.announce_(getString(
          'customBrowserNexusStatusConnected', 'Connected'));
      this.requestUpdate();
    } catch (error) {
      console.error('Failed to verify Nexus provider:', error);
      if (verifyToken !== this.verifyToken_) return;
      this.setStatusError_(
          getString('customBrowserNexusErrorUnexpectedResponse'));
      this.modelOptions_ = [];
      this.requestUpdate();
    }
  }

  private markConnectionDirty_() {
    if (!this.getCurrentProvider_()) return;
    this.verifyToken_++;
    this.errorCode_ = '';
    this.errorMessage_ = '';
    this.providerActionError_ = '';
    this.hasPendingProviderChanges_ = true;
    this.statusMode_ = 'draft';
  }

  private validateConnectionInputs_(): boolean {
    const provider = this.getCurrentProvider_();
    if (!provider) return false;

    if (this.supportsBaseUrl_(provider)) {
      const baseUrl = this.baseUrl_.trim();
      if (!baseUrl) {
        this.setStatusError_(getString('customBrowserNexusBaseUrlRequired'));
        return false;
      }
      const validationMessage = getBaseUrlValidationMessage(baseUrl);
      if (validationMessage) {
        this.setStatusError_(validationMessage, 'endpoint_invalid_url');
        return false;
      }
    }

    if (this.requiresApiKey_(provider) && !provider.isConfigured &&
        !this.hasStoredApiKey_ && !this.apiKey_.trim()) {
      // Don't show error — user hasn't entered anything yet
      return false;
    }

    return true;
  }

  // --- Event handlers ----------------------------------------------

  private toggleProviderDropdown_() {
    this.providerDropdownOpen_ = !this.providerDropdownOpen_;
    this.requestUpdate();
  }

  private closeProviderDropdown_() {
    this.providerDropdownOpen_ = false;
    this.requestUpdate();
  }

  private selectProvider_(providerId: string) {
    this.providerDropdownOpen_ = false;
    const provider = this.providers_.find(item => item.id === providerId);
    if (!provider) return;
    this.applyProviderSelection_(provider);
    // Always auto-verify on provider switch
    this.scheduleAutoVerify_();
  }

  private onBaseUrlInput_(event: Event) {
    this.baseUrl_ = (event.target as HTMLInputElement).value;
    this.markConnectionDirty_();
    this.requestUpdate();
    this.scheduleAutoVerify_();
  }

  private onApiKeyInput_(event: Event) {
    this.apiKey_ = (event.target as HTMLInputElement).value;
    this.markConnectionDirty_();
    this.requestUpdate();
    this.scheduleAutoVerify_();
  }

  private openResetDialog_() {
    if (!this.canResetProvider_()) {
      return;
    }
    this.resetDialogOpen_ = true;
    this.requestUpdate();
  }

  private closeResetDialog_() {
    if (this.resetting_) {
      return;
    }
    this.resetDialogOpen_ = false;
    this.requestUpdate();
  }

  private async onConfirmResetProviderClick_() {
    const provider = this.getCurrentProvider_();
    if (!provider || !this.resetDialogOpen_ || this.resetting_) {
      return;
    }

    this.resetting_ = true;
    this.providerActionError_ = '';
    this.requestUpdate();

    try {
      const resetResult = await sendWithPromise(
                              'resetProviderConfig',
                              provider.id) as ResetProviderConfigResult;
      if (!resetResult.success) {
        this.resetDialogOpen_ = false;
        this.providerActionError_ = getString('customBrowserNexusResetFailed');
        return;
      }

      this.resetDialogOpen_ = false;
      await this.loadProviders_();
      this.announce_(getStringF(
          'customBrowserNexusResetCompleted',
          '$1 connection reset.', getProviderDisplayName(provider)));
    } catch (error) {
      console.error('Failed to reset Nexus provider:', error);
      this.resetDialogOpen_ = false;
      this.providerActionError_ = getString('customBrowserNexusResetFailed');
    } finally {
      this.resetting_ = false;
      this.requestUpdate();
    }
  }

  // --- Render ------------------------------------------------------

  protected override render() {
    const provider = this.getCurrentProvider_();

    if (this.loading_) {
      return html`
        <nexus-subpage-layout>
        <section class="panel-shell">
          <div class="panel-card">
            <p class="loading-text">
              ${getString('customBrowserLoadingLabel', 'Loading…')}
            </p>
          </div>
        </section>
        </nexus-subpage-layout>
      `;
    }

    if (!this.providers_.length) {
      return html`
        <nexus-subpage-layout>
        <section class="panel-shell">
          <div class="panel-card">
            <h2 class="panel-title">
              ${getString('customBrowserNexusProviderPageTitle')}
            </h2>
            <p class="panel-description">
              ${getString('customBrowserNexusNoProviders')}
            </p>
          </div>
        </section>
        </nexus-subpage-layout>
      `;
    }

    const status = this.getStatusPresentation_();
    const providerSectionTitle = getString(
        'customBrowserNexusProviderPageTitle', 'Provider connection');
    const providerSectionDescription = getString(
        'customBrowserNexusProviderConnectionDescription',
        'Choose the service Nexus should connect to and verify the ' +
            'connection.');
    return html`
      <nexus-subpage-layout>
      <section class="panel-shell">
        <section class="settings-page-section">
          <div class="settings-page-section-header">
            <h2 class="settings-page-section-title">${providerSectionTitle}</h2>
          </div>
          <div class="section-card">
            <div class="card-row block">
              <div class="field">
                <label class="field-label with-description" id="providerSelectLabel">
                  ${getString('customBrowserNexusProviderLabel')}
                </label>
                <p class="card-row-description field-description">
                  ${providerSectionDescription}
                </p>
                <div class="custom-select">
                  <button class="custom-select-button"
                      aria-labelledby="providerSelectLabel"
                      @click=${this.toggleProviderDropdown_}>
                    ${provider ? html`
                      <img src="/components/nexus/${provider.id}.svg" class="provider-option-icon" alt="">
                      <span>${getProviderDisplayName(provider)}</span>
                    ` : html`<span>Select provider...</span>`}
                    <div class="chevron"></div>
                  </button>
                  ${this.providerDropdownOpen_ ? html`
                    <div class="dropdown-backdrop" @click=${this.closeProviderDropdown_}></div>
                    <div class="custom-select-dropdown" role="listbox">
                      ${this.providers_.map(providerOption => html`
                        <div class="custom-option ${providerOption.id === this.selectedProviderId_ ? 'selected' : ''}"
                            role="option"
                            aria-selected=${providerOption.id === this.selectedProviderId_}
                            @click=${() => this.selectProvider_(providerOption.id)}>
                          <img src="/components/nexus/${providerOption.id}.svg" class="provider-option-icon" alt="">
                          <span>${getProviderDisplayName(providerOption)}</span>
                        </div>
                      `)}
                    </div>
                  ` : nothing}
                </div>
              </div>

              ${provider && this.supportsBaseUrl_(provider) ? html`
                <div class="field">
                  <label class="field-label" for="baseUrlInput">
                    ${getString('customBrowserNexusBaseUrlLabel')}
                  </label>
                  <input id="baseUrlInput" class="field-control"
                      .value=${this.baseUrl_}
                      placeholder=${getString(
                          'customBrowserNexusBaseUrlPlaceholder')}
                      @input=${this.onBaseUrlInput_}>
                </div>
              ` : nothing}

              ${provider && this.requiresApiKey_(provider) ? html`
                <div class="field">
                  <label class="field-label" for="apiKeyInput">
                    ${getString('customBrowserNexusApiKeyLabel')}
                  </label>
                  <input id="apiKeyInput" class="field-control" type="password"
                      .value=${this.apiKey_}
                      placeholder=${getString(
                          'customBrowserNexusApiKeyPlaceholder')}
                      @input=${this.onApiKeyInput_}>
                  <p class="helper-text">
                    ${this.hasStoredApiKey_ && !this.apiKey_.trim() ?
                        getString('customBrowserNexusKeepStoredKey') :
                        getString('customBrowserNexusApiKeyHelper')}
                  </p>
                </div>
              ` : nothing}

              ${provider && this.getProviderNote_(provider) ? html`
                <p class="provider-note">
                  ${this.getProviderNote_(provider)}
                </p>
              ` : nothing}
            </div>

            <div class="card-row block">
              <div class="status-card">
                <div class="status-header">
                  <span class="status-chip ${status.tone}">${status.chip}</span>
                  <div class="status-copy">
                    <div class="status-title">${status.title}</div>
                    <div class="status-detail">${status.detail}</div>
                  </div>
                </div>
                ${this.statusMode_ === 'verifying' ?
                    html`<div class="status-progress"></div>` :
                    nothing}
              </div>
            </div>

            ${this.providerActionError_ ? html`
              <div class="card-row block">
                <p class="helper-text error">${this.providerActionError_}</p>
              </div>
            ` : nothing}

            ${provider && this.canResetProvider_() ? html`
              <div class="button-row">
                <cr-button class="action-button"
                    ?disabled=${this.resetting_}
                    @click=${this.openResetDialog_}>
                  ${getString(
                      'customBrowserNexusResetConnection',
                      'Reset connection')}
                </cr-button>
              </div>
            ` : nothing}
          </div>
        </section>
      </section>
      ${provider && this.resetDialogOpen_ ? html`
        <div class="dialog-backdrop"
            role="dialog"
            aria-modal="true"
            aria-labelledby="resetDialogTitle"
            @click=${this.closeResetDialog_}>
          <div class="dialog-card"
              @click=${(event: Event) => event.stopPropagation()}>
            <h3 class="dialog-title" id="resetDialogTitle">
              ${this.getResetDialogTitle_(provider)}
            </h3>
            <p class="dialog-body">
              ${this.getResetDialogBody_(provider)}
            </p>
            <div class="dialog-actions">
              <cr-button ?disabled=${this.resetting_}
                  @click=${this.closeResetDialog_}>
                ${getString('customBrowserCancel', 'Cancel')}
              </cr-button>
              <cr-button class="action-button"
                  ?disabled=${this.resetting_}
                  @click=${this.onConfirmResetProviderClick_}>
                ${getString(
                    'customBrowserNexusResetConnection',
                    'Reset connection')}
              </cr-button>
            </div>
          </div>
        </div>
      ` : nothing}
      <div class="sr-live" aria-live="polite" aria-atomic="true">
        ${this.liveAnnouncement_}
      </div>
      </nexus-subpage-layout>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'nexus-settings-models': NexusSettingsModelsElement;
  }
}

customElements.define(NexusSettingsModelsElement.is, NexusSettingsModelsElement);
