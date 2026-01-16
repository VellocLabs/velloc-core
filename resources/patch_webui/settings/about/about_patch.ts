import '/strings.m.js';
import {loadTimeData} from 'chrome://resources/js/load_time_data.js';

const VERSION_LINE_KEY = 'customBrowserVersionLine';
const CHROMIUM_LINE_KEY = 'customBrowserBasedOnChromiumLine';
const PATCHED_DATA_KEY = 'customBrowserVersionPatched';

type AboutPageElement = HTMLElement & {
  shadowRoot: ShadowRoot|null;
};

interface AboutPageState {
  versionNode: HTMLElement;
  originalText: string;
}

const stateMap = new WeakMap<AboutPageElement, AboutPageState>();

function getString(key: string): string {
  return loadTimeData.valueExists(key) ? loadTimeData.getString(key) : '';
}

function getStringF(key: string, ...args: Array<string>): string {
  return loadTimeData.valueExists(key) ? loadTimeData.getStringF(key, ...args) :
                                        '';
}

function buildVersionLine(): string {
  const productVersion = getString('customBrowserProductVersion');
  const official = getString('official');
  const modifier = getString('version_modifier');
  return getStringF(VERSION_LINE_KEY, productVersion, official, modifier);
}

function buildChromiumLine(): string {
  const chromiumVersion = getString('customBrowserChromiumVersion');
  return getStringF(CHROMIUM_LINE_KEY, chromiumVersion);
}

function findVersionNode(shadow: ShadowRoot): HTMLElement|null {
  return shadow.querySelector('div.cr-row.two-line .flex .secondary');
}

function attachPage(page: AboutPageElement) {
  const shadow = page.shadowRoot;
  if (!shadow) {
    return;
  }

  const versionNode = findVersionNode(shadow);
  if (!versionNode) {
    return;
  }

  if (versionNode.dataset[PATCHED_DATA_KEY] === 'true') {
    return;
  }

  const originalText = versionNode.textContent || '';
  versionNode.dataset[PATCHED_DATA_KEY] = 'true';
  stateMap.set(page, {versionNode, originalText});

  while (versionNode.firstChild) {
    versionNode.removeChild(versionNode.firstChild);
  }

  const firstLine = document.createElement('span');
  firstLine.textContent = buildVersionLine();

  const secondLine = document.createElement('span');
  secondLine.textContent = buildChromiumLine();

  versionNode.append(firstLine, document.createElement('br'), secondLine);
}

function detach(page: AboutPageElement) {
  const state = stateMap.get(page);
  if (!state) {
    return;
  }

  const {versionNode, originalText} = state;
  stateMap.delete(page);

  if (!versionNode.isConnected) {
    return;
  }

  while (versionNode.firstChild) {
    versionNode.removeChild(versionNode.firstChild);
  }
  versionNode.textContent = originalText;
  delete versionNode.dataset[PATCHED_DATA_KEY];
}

function scheduleAttach(page: AboutPageElement) {
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

function collectAboutPages(root: ParentNode, out: Set<AboutPageElement>) {
  if (!('querySelectorAll' in root)) {
    return;
  }

  for (const page of root.querySelectorAll('settings-about-page')) {
    out.add(page as AboutPageElement);
  }

  for (const element of root.querySelectorAll('*')) {
    const shadow = (element as HTMLElement).shadowRoot;
    if (!shadow) {
      continue;
    }
    observeShadowRoot(shadow);
    collectAboutPages(shadow, out);
  }
}

function scanAndAttach() {
  const pages = new Set<AboutPageElement>();
  collectAboutPages(document, pages);
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

function initializeAboutPatch() {
  customElements.whenDefined('settings-about-page').then(() => {
    const ctor = customElements.get('settings-about-page') as
        (new () => AboutPageElement)|undefined;
    if (!ctor) {
      return;
    }

    const proto = ctor.prototype as AboutPageElement & {
      connectedCallback?: () => void;
      disconnectedCallback?: () => void;
      __customBrowserAboutPatched?: boolean;
    };

    if (proto.__customBrowserAboutPatched) {
      return;
    }
    proto.__customBrowserAboutPatched = true;

    const originalConnected = proto.connectedCallback;
    proto.connectedCallback = function() {
      originalConnected?.call(this);
      scheduleAttach(this as AboutPageElement);
    };

    const originalDisconnected = proto.disconnectedCallback;
    proto.disconnectedCallback = function() {
      detach(this as AboutPageElement);
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
  initializeAboutPatch();
}
