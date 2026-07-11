import {
  CrLitElement,
  css,
  html,
  nothing,
  type CSSResultGroup,
} from 'chrome://resources/lit/v3_0/lit.rollup.js';

import {BrowserApiProxyImpl} from '../../browser_api_proxy.js';
import {BoolKey} from '../../browser_constants.mojom-webui.js';
import '../../components/native/native_view_aliases.js';

import './nexus_settings.js';
import './nexus_subpage_layout.js';
import {
  getInitialWarningAcknowledged,
  getString,
  type NexusPaneState,
  type NexusSettingsSection,
} from './nexus_shared.js';
import './nexus_view.js';
import './nexus_warning.js';
import {NexusTopbar, NexusTopbarMode} from './nexus_topbar.js';

export class NexusPage extends CrLitElement {
  static override styles: CSSResultGroup = css`
    :host {
      display: flex;
      flex-direction: column;
      flex: 1 1 auto;
      min-height: 0;
      width: 100%;
      height: 100%;
      align-items: stretch;
      box-sizing: border-box;
      overflow: hidden;
      background: var(--app-bg);
      color: var(--text-primary);
      -webkit-app-region: drag;
    }

    #topContainer {
      display: flex;
      flex-direction: row;
      padding: 2px 5px;
      width: 100%;
      border-bottom: 1px solid var(--app-bg);
      align-items: center;
    }

    #topContainer.settings-surface {
      background: var(--md-background-color);
      border-bottom-color: var(--md-background-color);
    }

    #topContainer nexus-topbar {
      flex: 1 1 auto;
      min-width: 0;
      -webkit-app-region: drag;
    }

    #pane {
      display: flex;
      flex-direction: column;
      flex: 1 1 auto;
      margin: var(--app-edge, 10px);
      min-height: 0;
      overflow: hidden;
    }

    #pane.settings-surface {
      background: var(--md-background-color);
    }

    #pane > * {
      display: flex;
      flex: 1 1 auto;
      min-height: 0;
    }

    #pane > [hidden] {
      display: none !important;
    }

  `;

  private paneState_: NexusPaneState =
      getInitialWarningAcknowledged() ? 'nexus-view' : 'warning';
  private selectedSettingsSection_: NexusSettingsSection = 'models';
  private readonly openSettingsFromHost_ = (section?: string) => {
    this.openSettings_(section === 'webapp' ? 'webapp' : 'models');
  };

  override connectedCallback(): void {
    super.connectedCallback();
    window.customBrowserOpenNexusSettings = this.openSettingsFromHost_;
  }

  override disconnectedCallback(): void {
    if (window.customBrowserOpenNexusSettings === this.openSettingsFromHost_) {
      delete window.customBrowserOpenNexusSettings;
    }
    super.disconnectedCallback();
  }

  override firstUpdated() {
    this.updateTopbarMode_();
  }

  private onWarningAccepted_() {
    BrowserApiProxyImpl.getInstance().saveBoolPref(
        BoolKey.kNexusWarningAcknowledged, true);
    this.paneState_ = 'nexus-view';
    this.requestUpdate();
    this.updateTopbarMode_();
  }

  private onSettingsClose_() {
    this.paneState_ = 'nexus-view';
    this.requestUpdate();
    this.updateTopbarMode_();
    this.getTopbar_()?.refresh();
  }

  private onOpenSettings_() {
    this.openSettings_('models');
  }

  private openSettings_(section: NexusSettingsSection = 'models') {
    this.selectedSettingsSection_ = section;
    this.paneState_ = 'settings';
    this.requestUpdate();
    this.updateTopbarMode_();
  }

  private onSettingsSectionChange_(
      event: CustomEvent<{section: NexusSettingsSection}>) {
    if (!event.detail?.section ||
        event.detail.section === this.selectedSettingsSection_) {
      return;
    }

    this.selectedSettingsSection_ = event.detail.section;
    this.requestUpdate();
    this.updateTopbarMode_();
  }

  private getSettingsSectionTitle_(): string {
    switch (this.selectedSettingsSection_) {
      case 'webapp':
        return getString('customBrowserNexusWebappPageTitle', 'Web App URL');
      case 'models':
      default:
        return getString(
            'customBrowserNexusProviderPageTitle', 'Model Providers');
    }
  }

  private getTopbar_(): NexusTopbar | null {
    return this.shadowRoot?.querySelector('nexus-topbar');
  }

  private updateTopbarMode_() {
    requestAnimationFrame(() => {
      const topbar = this.getTopbar_();
      if (!topbar) return;

      switch (this.paneState_) {
        case 'warning':
          topbar.setMode(NexusTopbarMode.TITLE, {
            icon: '🔒',
            title: 'Security Warning',
          });
          break;
        case 'settings':
          topbar.setMode(NexusTopbarMode.TITLE, {
            icon: '⚙️',
            closeLabel: getString(
                'customBrowserNexusCloseSettings', 'Close settings'),
            showCloseButton: true,
            title: `Settings - ${this.getSettingsSectionTitle_()}`,
          });
          break;
        case 'nexus-view':
          topbar.setMode(NexusTopbarMode.NEXUS_VIEW);
          break;
        default:
          break;
      }
    });
  }

  protected override render() {
    const showSettingsSurface = this.paneState_ === 'settings';
    const showTopbar = this.paneState_ !== 'nexus-view';
    return html`
      ${showTopbar ? html`
        <div id="topContainer" class=${showSettingsSurface ? 'settings-surface' : ''}>
          <nexus-topbar
              @nexus-open-settings=${this.onOpenSettings_}
              @nexus-settings-close=${this.onSettingsClose_}>
          </nexus-topbar>
        </div>
      ` : nothing}
      <div id="pane" class=${showSettingsSurface ? 'settings-surface' : ''}>
        <nexus-view ?hidden=${this.paneState_ !== 'nexus-view'}></nexus-view>
        ${this.paneState_ === 'warning' ? html`
          <nexus-warning
              @nexus-warning-accepted=${this.onWarningAccepted_}>
          </nexus-warning>
        ` : this.paneState_ === 'settings' ? html`
          <nexus-settings
              .selectedSection=${this.selectedSettingsSection_}
              @nexus-settings-close=${this.onSettingsClose_}
              @nexus-settings-models-complete=${this.onSettingsClose_}
              @nexus-settings-section-change=${this.onSettingsSectionChange_}>
          </nexus-settings>
        ` : nothing}
      </div>
    `;
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'nexus-page': NexusPage;
  }

  interface Window {
    customBrowserOpenNexusSettings?: (section?: string) => void;
  }
}

customElements.define('nexus-page', NexusPage);
