import {loadTimeData} from 'chrome://resources/js/load_time_data.js';

/*
This file is loaded on demand by the settings loader to add a Layout dropdown
to the Appearance page.
*/

const ROW_ID = 'customBrowserLayoutRow';
const LABEL_ID = 'customBrowserLayoutLabel';
const SELECT_ID = 'customBrowserLayoutSelect';

const LayoutPrefValue = {
  SIDEBAR: 1,
  COMPACT_SIDEBAR: 2,
  STANDARD: 3,
  COMPACT_STANDARD: 4,
} as const;

const ValidLayoutPrefValues =
    new Set<number>(Object.values(LayoutPrefValue));

const LAYOUT_PREF_NAME =
    loadTimeData.getString('customBrowserUIModePrefPath') ||
    'custom_browser.pref.kUIMode';

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

type AppearancePageElement = HTMLElement & {
  shadowRoot: ShadowRoot|null;
};

interface LayoutStrings {
  label: string;
  sidebar: string;
  compactSidebar: string;
  standard: string;
  compactStandard: string;
}

interface LayoutState {
  row: HTMLElement;
  select: HTMLSelectElement;
  prefListener: ((prefs: PrefObject[]) => void)|null;
  disabled: boolean;
  updating: boolean;
}

const stateMap = new WeakMap<AppearancePageElement, LayoutState>();

function getString(key: string): string {
  return loadTimeData.valueExists(key) ? loadTimeData.getString(key) : '';
}

function getLayoutStrings(): LayoutStrings {
  return {
    label: getString('modesHeader'),
    sidebar: getString('modeSidebar'),
    compactSidebar: getString('modeCompactSidebar'),
    standard: getString('modeStandard'),
    compactStandard: getString('modeCompactStandard'),
  };
}

function buildRow(strings: LayoutStrings) {
  const row = document.createElement('div');
  row.id = ROW_ID;
  row.className = 'cr-row';

  const label = document.createElement('div');
  label.id = LABEL_ID;
  label.className = 'flex cr-padded-text';
  label.textContent = strings.label;
  label.setAttribute('aria-hidden', 'true');

  const select = document.createElement('select');
  select.id = SELECT_ID;
  select.className = 'md-select';
  select.setAttribute('aria-labelledby', LABEL_ID);

  const options: Array<{value: number, name: string}> = [
    {value: LayoutPrefValue.SIDEBAR, name: strings.sidebar},
    {value: LayoutPrefValue.COMPACT_SIDEBAR, name: strings.compactSidebar},
    {value: LayoutPrefValue.STANDARD, name: strings.standard},
    // {value: LayoutPrefValue.COMPACT_STANDARD, name: strings.compactStandard},
  ];

  for (const option of options) {
    const element = document.createElement('option');
    element.value = String(option.value);
    element.textContent = option.name;
    select.appendChild(element);
  }

  row.append(label, select);
  return {row, select};
}

function findInsertionPoint(shadow: ShadowRoot): {parent: Node, before: ChildNode|null}|null {
  const colorSchemeRow = shadow.getElementById('colorSchemeModeRow');
  if (colorSchemeRow && colorSchemeRow.parentNode) {
    return {parent: colorSchemeRow.parentNode, before: colorSchemeRow};
  }

  const toolbarRow = shadow.getElementById('toolbarRow');
  if (toolbarRow && toolbarRow.parentNode) {
    return {parent: toolbarRow.parentNode, before: toolbarRow.nextSibling};
  }

  const routeDefault =
      shadow.querySelector<HTMLElement>('div[route-path="default"]');
  if (routeDefault) {
    return {parent: routeDefault, before: routeDefault.firstChild};
  }

  return null;
}

function createLayoutElements(page: AppearancePageElement): LayoutState|null {
  const shadow = page.shadowRoot;
  if (!shadow) {
    return null;
  }

  const existing = shadow.getElementById(ROW_ID);
  existing?.remove();

  const insertion = findInsertionPoint(shadow);
  if (!insertion) {
    return null;
  }

  const built = buildRow(getLayoutStrings());
  insertion.parent.insertBefore(built.row, insertion.before);

  const row = shadow.getElementById(ROW_ID);
  const select = shadow.getElementById(SELECT_ID) as HTMLSelectElement|null;
  if (!row || !select) {
    row?.remove();
    select?.remove();
    return null;
  }

  const state: LayoutState = {
    row,
    select,
    prefListener: null,
    disabled: false,
    updating: false,
  };

  select.addEventListener('change', () => {
    if (!chrome.settingsPrivate || state.updating || state.disabled) {
      return;
    }
    const value = Number(select.value);
    if (!ValidLayoutPrefValues.has(value)) {
      return;
    }
    chrome.settingsPrivate.setPref(LAYOUT_PREF_NAME, value);
  });

  return state;
}

function applyPref(state: LayoutState, pref: PrefObject) {
  const numericValue =
      typeof pref.value === 'number' ? pref.value : Number(pref.value);
  const value = ValidLayoutPrefValues.has(numericValue) ?
      numericValue :
      LayoutPrefValue.SIDEBAR;

  state.updating = true;
  if (state.select.value !== String(value)) {
    state.select.value = String(value);
  }
  state.updating = false;

  const disabled = pref.enforcement === 'ENFORCED';
  state.disabled = disabled;
  state.select.disabled = disabled;
}

function attachPage(page: AppearancePageElement): void {
  const shadow = page.shadowRoot;
  if (shadow && shadow.getElementById(ROW_ID) && stateMap.has(page)) {
    return;
  }

  detach(page);

  const state = createLayoutElements(page);
  if (!state) {
    return;
  }

  stateMap.set(page, state);

  if (!chrome.settingsPrivate) {
    state.select.value = String(LayoutPrefValue.SIDEBAR);
    return;
  }

  chrome.settingsPrivate.getPref(LAYOUT_PREF_NAME)
      .then(pref => {
        applyPref(state, pref);
      })
      .catch(() => {
        // Ignore failures; pref updates will be handled via the listener.
      });

  state.prefListener = (prefs: PrefObject[]) => {
    for (const pref of prefs) {
      if (pref.key === LAYOUT_PREF_NAME) {
        applyPref(state, pref);
        break;
      }
    }
  };
  chrome.settingsPrivate.onPrefsChanged.addListener(state.prefListener);
}

function detach(page: AppearancePageElement) {
  const state = stateMap.get(page);
  if (!state) {
    return;
  }

  state.row.remove();
  if (state.prefListener && chrome.settingsPrivate) {
    chrome.settingsPrivate.onPrefsChanged.removeListener(state.prefListener);
  }

  stateMap.delete(page);
}

function scheduleAttach(page: AppearancePageElement) {
  requestAnimationFrame(() => attachPage(page));
}

const observedShadows = new WeakSet<ShadowRoot>();
let scanScheduled = false;
let scanObserver: MutationObserver|null = null;

function observeShadowRoot(root: ShadowRoot) {
  if (observedShadows.has(root)) {
    return;
  }
  observedShadows.add(root);
  const observer = new MutationObserver(() => scheduleScan());
  observer.observe(root, {childList: true, subtree: true});
}

function collectAppearancePages(root: ParentNode, out: Set<AppearancePageElement>) {
  if (!('querySelectorAll' in root)) {
    return;
  }

  for (const page of root.querySelectorAll('settings-appearance-page')) {
    out.add(page as AppearancePageElement);
  }

  for (const element of root.querySelectorAll('*')) {
    const shadow = (element as HTMLElement).shadowRoot;
    if (!shadow) {
      continue;
    }
    observeShadowRoot(shadow);
    collectAppearancePages(shadow, out);
  }
}

function scanAndAttach() {
  const pages = new Set<AppearancePageElement>();
  collectAppearancePages(document, pages);
  for (const page of pages) {
    scheduleAttach(page);
  }
}

function scheduleScan() {
  if (scanScheduled) {
    return;
  }
  scanScheduled = true;
  requestAnimationFrame(() => {
    scanScheduled = false;
    scanAndAttach();
  });
}

function startScanning() {
  if (scanObserver) {
    return;
  }
  const root = document.body || document.documentElement;
  if (!root) {
    requestAnimationFrame(startScanning);
    return;
  }
  scanObserver = new MutationObserver(() => scheduleScan());
  scanObserver.observe(root, {childList: true, subtree: true});
  scheduleScan();
}

let initialized = false;

function initializeAppearancePatch() {
  customElements.whenDefined('settings-appearance-page').then(() => {
    const ctor = customElements.get('settings-appearance-page') as
        (new () => AppearancePageElement)|undefined;
    if (!ctor) {
      return;
    }

    const proto = ctor.prototype as AppearancePageElement & {
      connectedCallback?: () => void;
      disconnectedCallback?: () => void;
      __customBrowserLayoutPatched?: boolean;
    };

    if (proto.__customBrowserLayoutPatched) {
      return;
    }
    proto.__customBrowserLayoutPatched = true;

    const originalConnected = proto.connectedCallback;
    proto.connectedCallback = function() {
      originalConnected?.call(this);
      scheduleAttach(this as AppearancePageElement);
    };

    const originalDisconnected = proto.disconnectedCallback;
    proto.disconnectedCallback = function() {
      detach(this as AppearancePageElement);
      originalDisconnected?.call(this);
    };

    startScanning();
  });

  customElements.whenDefined('settings-ui').then(() => startScanning());
  startScanning();
}

export function attach() {
  if (initialized) {
    return;
  }
  initialized = true;
  initializeAppearancePatch();
}
