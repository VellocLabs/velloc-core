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
import {
  getInitialWarningAcknowledged,
  type NexusPaneState,
  type NexusSettingsSection,
} from './nexus_shared.js';
import './nexus_view.js';
import type {NexusViewElement} from './nexus_view.js';
import './nexus_warning.js';
import './nexus_topbar.js';
import type {NexusTopbarSurfaceMode} from './nexus_topbar.js';
// Session-list custom element. Registered for global use so any surface
// inside this page (today: nothing renders it by default; the user can
// place `<nexus-session-list>` wherever the multi-session sidebar lives)
// receives `nexusSessionsSummary` updates from the SharedWorker.
import './nexus_session_list.js';

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
    }

    nexus-topbar {
      flex: 0 0 auto;
    }

    #pane {
      display: flex;
      flex-direction: column;
      flex: 1 1 auto;
      margin: 0;
      min-height: 0;
      overflow: hidden;
    }

    #pane.settings-surface {
      background: var(--md-background-color);
      margin: 0;
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
  // Mirror of <nexus-view>'s agentZoneVisible_, kept in sync via the
  // 'nexus-agent-zone-changed' event so we can push the value to the
  // topbar's `agentZoneVisible` reactive prop.
  private agentZoneVisible_ = false;
  private readonly openSettingsFromHost_ = (section?: string) => {
    this.openSettings_(this.normalizeSection_(section));
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

  private normalizeSection_(section?: string): NexusSettingsSection {
    // Legacy alias from C++ host: 'webapp' now lives under About.
    if (section === 'webapp') {
      return 'about';
    }
    if (section === 'mcp' || section === 'permissions' ||
        section === 'tools' || section === 'shortcuts' ||
        section === 'about') {
      return section;
    }
    return 'models';
  }

  private onWarningAccepted_() {
    BrowserApiProxyImpl.getInstance().saveBoolPref(
        BoolKey.kNexusWarningAcknowledged, true);
    this.paneState_ = 'nexus-view';
    this.requestUpdate();
  }

  private onSettingsClose_() {
    this.paneState_ = 'nexus-view';
    this.requestUpdate();
  }

  private onOpenSettings_(event: Event) {
    const detail = (event as CustomEvent<unknown>).detail as
        {section?: string} | undefined;
    this.openSettings_(this.normalizeSection_(detail?.section));
  }

  private openSettings_(section: NexusSettingsSection = 'models') {
    this.selectedSettingsSection_ = section;
    this.paneState_ = 'settings';
    this.requestUpdate();
  }

  private onSettingsSectionChange_(
      event: CustomEvent<{section: NexusSettingsSection}>) {
    if (!event.detail?.section ||
        event.detail.section === this.selectedSettingsSection_) {
      return;
    }

    this.selectedSettingsSection_ = event.detail.section;
    this.requestUpdate();
  }

  // Forward the topbar's "+" button click to anything that wants to listen.
  // The React guest inside <nexus-view-container> is the intended consumer
  // (a future pass will route this through the bridge port). For now we
  // just log so the wiring is observable in DevTools.
  // TODO(nexus-new-thread): forward to React guest via bridge port.
  private onNewThread_() {
    console.debug('[nexus] new thread requested');
  }

  // Topbar's agent-zone toggle (globe button) → forward to <nexus-view>.
  // <nexus-view> mutates its flag and fires 'nexus-agent-zone-changed'
  // which we receive in onAgentZoneChanged_ to mirror into the topbar's
  // `agentZoneVisible` reactive prop.
  private onToggleAgentZone_() {
    const view =
        this.shadowRoot?.querySelector<NexusViewElement>('nexus-view');
    view?.toggleAgentZone();
  }

  // Topbar fires this on the leading edge of every browser-category
  // tool dispatch (see `ToolsAdapter` → `nexusAgentStateChanged` ->
  // `isBrowserTool`). One-direction: reveals the zone without writing
  // through to the saved preference. There is no paired hide — the
  // zone stays open until the user manually clicks the globe.
  private onShowAgentZoneTransient_() {
    const view =
        this.shadowRoot?.querySelector<NexusViewElement>('nexus-view');
    view?.showAgentZoneTransient();
  }

  private onAgentZoneChanged_(event: Event) {
    const detail = (event as CustomEvent<unknown>).detail as
        {visible?: boolean} | undefined;
    this.agentZoneVisible_ = !!detail?.visible;
    this.requestUpdate();
  }

  private getSurfaceMode_(): NexusTopbarSurfaceMode {
    switch (this.paneState_) {
      case 'warning':
        return 'warning';
      case 'settings':
        return 'settings';
      case 'nexus-view':
      default:
        return 'nexus-view';
    }
  }

  protected override render() {
    const showSettingsSurface = this.paneState_ === 'settings';
    return html`
      <nexus-topbar
          .surfaceMode=${this.getSurfaceMode_()}
          .agentZoneVisible=${this.agentZoneVisible_}
          @nexus-open-settings=${this.onOpenSettings_}
          @nexus-settings-close=${this.onSettingsClose_}
          @nexus-new-thread=${this.onNewThread_}
          @nexus-toggle-agent-zone=${this.onToggleAgentZone_}
          @nexus-show-agent-zone-transient=${this.onShowAgentZoneTransient_}>
      </nexus-topbar>
      <div id="pane" class=${showSettingsSurface ? 'settings-surface' : ''}>
        <nexus-view
            ?hidden=${this.paneState_ !== 'nexus-view'}
            @nexus-agent-zone-changed=${this.onAgentZoneChanged_}>
        </nexus-view>
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
