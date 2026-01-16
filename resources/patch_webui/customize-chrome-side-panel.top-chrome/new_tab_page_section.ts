import 'chrome://resources/cr_elements/cr_chip/cr_chip.js';
import 'chrome://resources/cr_elements/cr_icon/cr_icon.js';
import 'chrome://resources/cr_elements/cr_icons.css.js';
import 'chrome://resources/cr_elements/cr_shared_vars.css.js';
import 'chrome://resources/cr_elements/cr_toggle/cr_toggle.js';
import {loadTimeData} from 'chrome://resources/js/load_time_data.js';
import type {CrToggleElement} from
    'chrome://resources/cr_elements/cr_toggle/cr_toggle.js';
import {CrLitElement, html} from
    'chrome://resources/lit/v3_0/lit.rollup.js';

const SECTION_ID = 'customBrowserNtpSection';
const SEPARATOR_ID = 'customBrowserNtpSeparator';
const SHOW_SEARCH_TOGGLE_ID = 'customBrowserNtpShowSearchToggle';
const SHOW_LOGO_TOGGLE_ID = 'customBrowserNtpShowLogoToggle';

const SHOW_SEARCH_PREF_PATH =
    loadTimeData.valueExists('customNewTabShowSearchPrefPath') ?
    loadTimeData.getString('customNewTabShowSearchPrefPath') :
    'custom_browser.pref.new_tab.show_search';
const SHOW_LOGO_PREF_PATH =
    loadTimeData.valueExists('customNewTabShowLogoPrefPath') ?
    loadTimeData.getString('customNewTabShowLogoPrefPath') :
    'custom_browser.pref.new_tab.show_logo';
interface PrefObject {
  key: string;
  value: unknown;
  enforcement?: string;
}

interface SettingsPrivate {
  getPref(name: string): Promise<PrefObject>;
  setPref(name: string, value: unknown): Promise<boolean>;
  onPrefsChanged: {
    addListener(callback: (prefs: PrefObject[]) => void): void;
    removeListener(callback: (prefs: PrefObject[]) => void): void;
  };
}

declare const chrome: {
  settingsPrivate?: SettingsPrivate;
};

interface NewTabPageStrings {
  header: string;
  showSearch: string;
  showLogo: string;
  searchEngine: string;
}

type CustomizeChromeAppElement = HTMLElement & {
  pageHandler_?: {
    openNtpManagedByPage?: () => void;
  };
};

function getStrings(): NewTabPageStrings {
  const defaults = {
    header: '',
    showSearch: '',
    showLogo: '',
    searchEngine: '',
  };

  return {
    header: loadTimeData.valueExists('customizeChromeNtpSectionTitle') ?
        loadTimeData.getString('customizeChromeNtpSectionTitle') :
        defaults.header,
    showSearch: loadTimeData.valueExists('customizeChromeNtpShowSearchLabel') ?
        loadTimeData.getString('customizeChromeNtpShowSearchLabel') :
        defaults.showSearch,
    showLogo: loadTimeData.valueExists('customizeChromeNtpShowLogoLabel') ?
        loadTimeData.getString('customizeChromeNtpShowLogoLabel') :
        defaults.showLogo,
    searchEngine:
        loadTimeData.valueExists('customizeChromeNtpSearchEngineLabel') ?
        loadTimeData.getString('customizeChromeNtpSearchEngineLabel') :
        defaults.searchEngine,
  };
}

export class NewTabPageSection extends CrLitElement {
  static get is() {
    return 'custom-browser-new-tab-page-section';
  }

  // Render into light DOM so existing Customize Chrome card styles apply.
  override createRenderRoot() {
    return this;
  }

  static override properties = {
    showSearchPrefPath: {type: String},
    showLogoPrefPath: {type: String},
    searchDisabled_: {type: Boolean, state: true},
    showSearch_: {type: Boolean, state: true},
    logoDisabled_: {type: Boolean, state: true},
    showLogo_: {type: Boolean, state: true},
    strings_: {type: Object},
  };

  showSearchPrefPath: string = SHOW_SEARCH_PREF_PATH;
  showLogoPrefPath: string = SHOW_LOGO_PREF_PATH;

  private prefListener_: ((prefs: PrefObject[]) => void)|null = null;
  private searchDisabled_: boolean = false;
  private showSearch_: boolean = true;
  private logoDisabled_: boolean = false;
  private showLogo_: boolean = true;
  private strings_: NewTabPageStrings = getStrings();

  override connectedCallback() {
    super.connectedCallback();
    this.startPrefSync_();
  }

  override disconnectedCallback() {
    this.stopPrefSync_();
    super.disconnectedCallback();
  }

  protected override render() {
    const strings = this.strings_;
    return html`
      <div id=${SECTION_ID} class="section sp-card">
        <sp-heading hide-back-button>
          <h2 id="customBrowserNtpHeading" slot="heading">
            ${strings.header}
          </h2>
        </sp-heading>
        <div class="sp-card-content">
          <div class="toggle-row"
              @click=${(event: Event) => this.onSearchToggleRowClick_(event)}>
            <div class="toggle-label">${strings.showSearch}</div>
            <cr-toggle id=${SHOW_SEARCH_TOGGLE_ID}
                .checked=${this.showSearch_}
                @change=${this.onSearchToggle_}
                ?disabled=${this.searchDisabled_}>
            </cr-toggle>
          </div>
          <div class="toggle-row"
              @click=${(event: Event) => this.onLogoToggleRowClick_(event)}>
            <div class="toggle-label">${strings.showLogo}</div>
            <cr-toggle id=${SHOW_LOGO_TOGGLE_ID}
                .checked=${this.showLogo_}
                @change=${this.onLogoToggle_}
                ?disabled=${this.logoDisabled_}>
            </cr-toggle>
          </div>
          <div class="action-row">
            <cr-chip chip-role="link"
                @click=${(event: Event) =>
                    this.onOpenSearchSettings_(event)}>
              <span class="chip-inner">
                <cr-icon icon="cr:search" aria-hidden="true"></cr-icon>
                <span class="chip-label">${strings.searchEngine}</span>
              </span>
            </cr-chip>
          </div>
        </div>
      </div>
      <hr id=${SEPARATOR_ID} class="sp-cards-separator">
    `;
  }

  private startPrefSync_() {
    const settingsPrivate = chrome.settingsPrivate;
    if (!settingsPrivate) {
      return;
    }

    const prefPaths = [this.showSearchPrefPath, this.showLogoPrefPath];
    prefPaths.forEach(path => {
      settingsPrivate.getPref(path)
          .then(pref => this.applyPref_(pref))
          .catch(() => {
            // Ignore failures; listener will handle future updates.
          });
    });

    this.prefListener_ = (prefs: PrefObject[]) => {
      for (const pref of prefs) {
        this.applyPref_(pref);
      }
    };
    settingsPrivate.onPrefsChanged.addListener(this.prefListener_);
  }

  private stopPrefSync_() {
    if (this.prefListener_ && chrome.settingsPrivate) {
      chrome.settingsPrivate.onPrefsChanged.removeListener(
          this.prefListener_);
    }
    this.prefListener_ = null;
  }

  private applyPref_(pref: PrefObject) {
    if (pref.key === this.showSearchPrefPath) {
      this.showSearch_ = !!pref.value;
      this.searchDisabled_ = pref.enforcement === 'ENFORCED';
    } else if (pref.key === this.showLogoPrefPath) {
      this.showLogo_ = !!pref.value;
      this.logoDisabled_ = pref.enforcement === 'ENFORCED';
    }
    this.requestUpdate();
  }

  private onSearchToggle_(event: Event) {
    const toggle = event.target as CrToggleElement;
    const settingsPrivate = chrome.settingsPrivate;
    if (!settingsPrivate || this.searchDisabled_) {
      toggle.checked = this.showSearch_;
      return;
    }
    const nextValue = toggle.checked;
    this.showSearch_ = nextValue;
    settingsPrivate.setPref(this.showSearchPrefPath, nextValue)
        .then(success => {
          if (!success) {
            this.showSearch_ = !nextValue;
          }
        });
  }

  private onLogoToggle_(event: Event) {
    const toggle = event.target as CrToggleElement;
    const settingsPrivate = chrome.settingsPrivate;
    if (!settingsPrivate || this.logoDisabled_) {
      toggle.checked = this.showLogo_;
      return;
    }
    const nextValue = toggle.checked;
    this.showLogo_ = nextValue;
    settingsPrivate.setPref(this.showLogoPrefPath, nextValue)
        .then(success => {
          if (!success) {
            this.showLogo_ = !nextValue;
          }
        });
  }

  private onOpenSearchSettings_(event: Event) {
    event.preventDefault();
    const root = this.getRootNode();
    const host =
        root instanceof ShadowRoot ? root.host as CustomizeChromeAppElement :
                                     null;
    const handler = host?.pageHandler_;
    if (handler?.openNtpManagedByPage) {
      handler.openNtpManagedByPage();
      return;
    }
    window.open('chrome://settings/searchEngines', '_blank', 'noopener');
  }

  private isToggleClick_(event: Event): boolean {
    const path = event.composedPath();
    for (const target of path) {
      if (target instanceof HTMLElement &&
          target.tagName === 'CR-TOGGLE') {
        return true;
      }
    }
    return false;
  }

  private onSearchToggleRowClick_(event: Event) {
    const settingsPrivate = chrome.settingsPrivate;
    if (!settingsPrivate || this.isToggleClick_(event)) {
      return;
    }
    if (this.searchDisabled_) {
      return;
    }
    const nextValue = !this.showSearch_;
    this.showSearch_ = nextValue;
    settingsPrivate.setPref(this.showSearchPrefPath, nextValue)
        .then(success => {
          if (!success) {
            this.showSearch_ = !nextValue;
          }
        });
  }

  private onLogoToggleRowClick_(event: Event) {
    const settingsPrivate = chrome.settingsPrivate;
    if (!settingsPrivate || this.isToggleClick_(event)) {
      return;
    }
    if (this.logoDisabled_) {
      return;
    }
    const nextValue = !this.showLogo_;
    this.showLogo_ = nextValue;
    settingsPrivate.setPref(this.showLogoPrefPath, nextValue)
        .then(success => {
          if (!success) {
            this.showLogo_ = !nextValue;
          }
        });
  }
}

customElements.define(NewTabPageSection.is, NewTabPageSection);
