import 'chrome://resources/cr_elements/cr_radio_button/cr_radio_button.js';
import 'chrome://resources/cr_elements/cr_radio_group/cr_radio_group.js';
import {loadTimeData} from 'chrome://resources/js/load_time_data.js';
import {CrLitElement, html} from 'chrome://resources/lit/v3_0/lit.rollup.js';
import type {PropertyValues} from 'chrome://resources/lit/v3_0/lit.rollup.js';

const SECTION_ID = 'customBrowserModesSection';
const SEPARATOR_ID = 'customBrowserModesSeparator';
const GROUP_ID = 'customBrowserModesGroup';

const LayoutRadioName = {
  SIDEBAR: 'sidebar',
  COMPACT_SIDEBAR: 'compactSidebar',
  STANDARD: 'standard',
  COMPACT_STANDARD: 'compactStandard',
} as const;

const LayoutRadioToPrefValue = new Map<string, number>([
  [LayoutRadioName.SIDEBAR, 1],
  [LayoutRadioName.COMPACT_SIDEBAR, 2],
  [LayoutRadioName.STANDARD, 3],
  [LayoutRadioName.COMPACT_STANDARD, 4],
]);

const PrefValueToLayoutRadio =
    new Map<number, string>(Array.from(LayoutRadioToPrefValue.entries())
                                .map(([name, value]) => [value, name]));

const LAYOUT_PREF_NAME =
    loadTimeData.valueExists('customBrowserUIModePrefPath') ?
    loadTimeData.getString('customBrowserUIModePrefPath') :
    'custom_browser.pref.kUIMode';

interface PrefObject {
  key: string;
  value: unknown;
  enforcement?: string;
}

interface SettingsPrivate {
  // callback-style (real API)
  getPref(name: string, callback: (pref: PrefObject) => void): void;

  setPref(name: string, value: unknown): Promise<boolean>;
  onPrefsChanged: {
    addListener(callback: (prefs: PrefObject[]) => void): void;
    removeListener(callback: (prefs: PrefObject[]) => void): void;
  };
}

declare const chrome: {
  settingsPrivate?: SettingsPrivate;
};

interface LayoutStrings {
  header: string;
  sidebar: string;
  sidebarDescription: string;
  compactSidebar: string;
  compactSidebarDescription: string;
  standard: string;
  standardDescription: string;
  compactStandard: string;
  compactStandardDescription: string;
  loadingLabel: string;
}

type LayoutSelectionChangedEvent = CustomEvent<{selection: string}>;

type LayoutOption = {
  containerName: string;
  buttonName: string;
  type: string;
  title: string;
  description: string;
};

function getString(key: string, fallback: string): string {
  return loadTimeData.valueExists(key) ? loadTimeData.getString(key) : fallback;
}

function getLayoutStrings(): LayoutStrings {
  return {
    header: getString('modesHeader', ''),
    sidebar: getString('modeSidebar', ''),
    sidebarDescription: getString('modeSidebarDescription', ''),
    compactSidebar: getString('modeCompactSidebar', ''),
    compactSidebarDescription: getString('modeCompactSidebarDescription', ''),
    standard: getString('modeStandard', ''),
    standardDescription: getString('modeStandardDescription', ''),
    compactStandard: getString('modeCompactStandard', ''),
    compactStandardDescription: getString('modeCompactStandardDescription', ''),
    loadingLabel: getString('customBrowserLoadingLabel', ''),
  };
}

function getLayoutOptions(strings: LayoutStrings): LayoutOption[] {
  return [
    {
      containerName: 'customBrowserLayoutSidebarOption',
      buttonName: LayoutRadioName.SIDEBAR,
      type: LayoutRadioName.SIDEBAR,
      title: strings.sidebar,
      description: strings.sidebarDescription,
    },
    {
      containerName: 'customBrowserLayoutCompactSidebarOption',
      buttonName: LayoutRadioName.COMPACT_SIDEBAR,
      type: LayoutRadioName.COMPACT_SIDEBAR,
      title: strings.compactSidebar,
      description: strings.compactSidebarDescription,
    },
    {
      containerName: 'customBrowserLayoutStandardOption',
      buttonName: LayoutRadioName.STANDARD,
      type: LayoutRadioName.STANDARD,
      title: strings.standard,
      description: strings.standardDescription,
    },
    // {
    //   containerName: 'customBrowserLayoutCompactStandardOption',
    //   buttonName: LayoutRadioName.COMPACT_STANDARD,
    //   type: LayoutRadioName.COMPACT_STANDARD,
    //   title: strings.compactStandard,
    //   description: strings.compactStandardDescription,
    // },
  ];
}

// Minimal typing for <cr-radio-group> so we can use .selected without `any`.
interface CrRadioGroupElement extends HTMLElement {
  selected: string;
}

export class LayoutSection extends CrLitElement {
  static get is() {
    return 'custom-browser-layout-section';
  }

  override createRenderRoot() {
    return this;
  }

  static override properties = {
    prefPath: {type: String},
    disabled_: {type: Boolean, state: true},
    selection_: {type: String, state: true},
    strings_: {type: Object},
    prefLoaded_: {type: Boolean, state: true},
  };

  prefPath: string = LAYOUT_PREF_NAME;
  private prefListener_: ((prefs: PrefObject[]) => void)|null = null;

  private disabled_: boolean = false;
  private selection_: string = LayoutRadioName.SIDEBAR;
  private strings_: LayoutStrings = getLayoutStrings();

  // Key fix: do not render radio-group until pref value is loaded.
  private prefLoaded_: boolean = false;

  // Suppress `selected-changed` while we apply pref / force group.selected.
  private applyingPref_ = false;

  private lastDispatchedSelection_: string|null = null;

  override connectedCallback() {
    super.connectedCallback();
    this.startPrefSync_();
  }

  override disconnectedCallback() {
    this.stopPrefSync_();
    super.disconnectedCallback();
  }

  private getGroup_(): CrRadioGroupElement|null {
    return this.querySelector<CrRadioGroupElement>(`#${GROUP_ID}`);
  }

  override updated(changedProperties: PropertyValues<this>) {
    super.updated(changedProperties);

    // Always reconcile (cheap) to beat any init timing where the group picks the first option.
    if (!this.prefLoaded_ || this.applyingPref_) {
      return;
    }

    const group = this.getGroup_();
    if (group && group.selected !== this.selection_) {
      group.selected = this.selection_;
    }
  }

  protected override render() {
    const strings = this.strings_;
    const options = getLayoutOptions(strings);

    return html`
      <div id=${SECTION_ID} class="section sp-card">
        <sp-heading hide-back-button>
          <h2 id="customBrowserModesHeading" slot="heading">
            ${strings.header}
          </h2>
        </sp-heading>

        <div class="sp-card-content layout-options">
          ${this.prefLoaded_ ? html`
            <cr-radio-group id=${GROUP_ID} nested-selectable
                .selected=${this.selection_}
                @selected-changed=${this.onSelectionChanged_}
                ?disabled=${this.disabled_}>
              ${options.map(item => html`
                <div class="option" id=${item.containerName}
                    @click=${() => this.onOptionClick_(item)}>
                  <customize-chrome-button-label
                      .label=${item.title}
                      .labelDescription=${item.description}>
                  </customize-chrome-button-label>

                  <cr-radio-button
                      .name=${item.buttonName}
                      .label=${item.title}
                      hide-label-text>
                    <span class="button-aria-describedby">
                      ${item.description}
                    </span>
                  </cr-radio-button>
                </div>
              `)}
            </cr-radio-group>
          ` : html`
            <div aria-busy="true" style="padding: 12px 0;">
              ${strings.loadingLabel}
            </div>
          `}
        </div>
      </div>

      <hr id=${SEPARATOR_ID} class="sp-cards-separator">
    `;
  }

  // Row click: only change selection. onSelectionChanged_ is the only writer to prefs.
  private onOptionClick_(option: LayoutOption) {
    if (this.disabled_ || !this.prefLoaded_) {
      return;
    }
    const group = this.getGroup_();
    if (group) {
      group.selected = option.type;
    }
  }

  private onSelectionChanged_(event: CustomEvent<{value: string}>) {
    const newSelection = event.detail?.value;

    // Ignore init/programmatic churn.
    if (!this.prefLoaded_ || this.applyingPref_) {
      return;
    }
    if (!newSelection || newSelection === this.selection_) {
      return;
    }

    this.selection_ = newSelection;
    this.dispatchSelectionChanged_();

    const prefValue = LayoutRadioToPrefValue.get(newSelection);
    const settingsPrivate = this.getSettingsPrivate_();
    if (prefValue === undefined || this.disabled_ || !settingsPrivate) {
      return;
    }

    console.error('onSelectionChanged_', newSelection, prefValue);
    void settingsPrivate.setPref(this.prefPath, prefValue);
  }

  private dispatchSelectionChanged_() {
    if (this.selection_ === this.lastDispatchedSelection_) {
      return;
    }
    this.lastDispatchedSelection_ = this.selection_;
    const event: LayoutSelectionChangedEvent =
        new CustomEvent('layout-selection-changed', {
          bubbles: true,
          composed: true,
          detail: {selection: this.selection_},
        });
    this.dispatchEvent(event);
  }

  private getSettingsPrivate_(): SettingsPrivate|undefined {
    const globalChrome = typeof chrome !== 'undefined' ? chrome : undefined;
    return globalChrome?.settingsPrivate;
  }

  private getPrefPromise_(settingsPrivate: SettingsPrivate,
      name: string): Promise<PrefObject> {
    return new Promise((resolve) => {
      settingsPrivate.getPref(name, resolve);
    });
  }

  private async startPrefSync_() {
    const settingsPrivate = this.getSettingsPrivate_();
    if (!settingsPrivate) {
      this.selection_ = LayoutRadioName.SIDEBAR;
      this.disabled_ = false;
      this.prefLoaded_ = true;
      this.dispatchSelectionChanged_();
      this.requestUpdate();
      return;
    }

    try {
      const pref = await this.getPrefPromise_(settingsPrivate, this.prefPath);
      this.applyPref_(pref);
    } catch (e) {
      console.error('getPref failed', e);
      this.selection_ = LayoutRadioName.SIDEBAR;
      this.disabled_ = false;
      this.dispatchSelectionChanged_();
    } finally {
      // IMPORTANT: never get stuck on Loading…
      this.prefLoaded_ = true;
      this.requestUpdate();
    }

    this.prefListener_ = (prefs: PrefObject[]) => {
      for (const pref of prefs) {
        if (pref.key === this.prefPath) {
          this.applyPref_(pref);
          break;
        }
      }
    };
    settingsPrivate.onPrefsChanged.addListener(this.prefListener_);
  }

  private stopPrefSync_() {
    const settingsPrivate = this.getSettingsPrivate_();
    if (this.prefListener_ && settingsPrivate) {
      settingsPrivate.onPrefsChanged.removeListener(this.prefListener_);
    }
    this.prefListener_ = null;
  }

  private applyPref_(pref: PrefObject) {
    const numericValue =
        typeof pref.value === 'number' ? pref.value : Number(pref.value);

    const selection =
        PrefValueToLayoutRadio.get(numericValue) ?? LayoutRadioName.SIDEBAR;

    console.error('Applying layout pref:', pref.key, pref.value, selection);

    // Keep suppressed until group exists and we force its selected value once.
    this.applyingPref_ = true;

    this.selection_ = selection;
    this.disabled_ = pref.enforcement === 'ENFORCED';
    this.prefLoaded_ = true;

    this.dispatchSelectionChanged_();
    // Ensure we re-render if this was applied after the initial "Loading…" render.
    this.requestUpdate();

    void this.updateComplete.then(() => {
      const group = this.getGroup_();
      if (group && group.selected !== this.selection_) {
        group.selected = this.selection_;
      }
      this.applyingPref_ = false;
    });
  }
}

customElements.define(LayoutSection.is, LayoutSection);
