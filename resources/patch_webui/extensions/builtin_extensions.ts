import {sendWithPromise} from 'chrome://resources/js/cr.js';
import {loadTimeData} from 'chrome://resources/js/load_time_data.js';

declare const chrome: any;

interface BuiltinExtensionEntry {
  name: string;
  displayName?: string;
  description?: string;
  version?: string;
  id?: string;
  enabled: boolean;
  enabledByDefault: boolean;
  hasBackgroundPage: boolean;
}

interface BuiltinDetails extends BuiltinExtensionEntry {
  id: string;
  displayName: string;
  description: string;
  version: string;
}

type ExtensionInfo = any;
type Filter = (info: ExtensionInfo) => boolean;

type ItemListElement = HTMLElement & {
  extensions: ExtensionInfo[];
  apps: ExtensionInfo[];
  delegate: any;
  requestUpdate?: (prop?: string) => void;
};

const listElements = new Set<ItemListElement>();
const baseExtensionsMap = new WeakMap<ItemListElement, ExtensionInfo[]>();
const updatingLists = new WeakSet<ItemListElement>();
const builtinIdToName = new Map<string, string>();
const builtinDetailsById = new Map<string, BuiltinDetails>();
const builtinIds = new Set<string>();
let builtinInfos: ExtensionInfo[] = [];
let builtinsPromise: Promise<void>|null = null;
let listExtensionsSetter:
    ((this: ItemListElement, value: ExtensionInfo[]) => void)|null = null;
let listObserver: MutationObserver|null = null;
let attachScheduled = false;
let attachRetryCount = 0;
let builtinDetailsDialog: HTMLDialogElement|null = null;
let builtinDetailsBody: HTMLElement|null = null;
const shadowObservers = new WeakMap<ShadowRoot, MutationObserver>();
const DEBUG = true;

function getString(key: string): string {
  return loadTimeData.valueExists(key) ? loadTimeData.getString(key) : '';
}

const builtinStrings = {
  dialogTitle: getString('customBrowserBuiltinExtensionTitle'),
  closeLabel: getString('customBrowserCloseLabel'),
  nameLabel: getString('customBrowserBuiltinDetailsName'),
  descriptionLabel: getString('customBrowserBuiltinDetailsDescription'),
  versionLabel: getString('customBrowserBuiltinDetailsVersion'),
  idLabel: getString('customBrowserBuiltinDetailsId'),
  enabledLabel: getString('customBrowserBuiltinDetailsEnabled'),
  defaultLabel: getString('customBrowserBuiltinDetailsDefault'),
  backgroundPageLabel: getString('customBrowserBuiltinDetailsBackgroundPage'),
  valueOn: getString('customBrowserValueOn'),
  valueOff: getString('customBrowserValueOff'),
  valueYes: getString('customBrowserValueYes'),
  valueNo: getString('customBrowserValueNo'),
  valueNa: getString('customBrowserValueNa'),
};

function debugLog(...args: unknown[]) {
  if (!DEBUG) {
    return;
  }
  // eslint-disable-next-line no-console
  console.info('[custom-builtins]', ...args);
}

function setListExtensions(
    list: ItemListElement,
    setter: ((value: ExtensionInfo[]) => void)|undefined,
    extensions: ExtensionInfo[]) {
  if (!setter) {
    debugLog('setListExtensions skip: missing setter');
    return;
  }
  updatingLists.add(list);
  setter.call(list, extensions);
  updatingLists.delete(list);
  if (list.requestUpdate) {
    list.requestUpdate('extensions');
    list.requestUpdate('filter');
    requestAnimationFrame(() => list.requestUpdate?.('extensions'));
  }
  debugLog('setListExtensions', {
    count: extensions.length,
    tag: list.tagName,
  });
}

function refreshList(
    list: ItemListElement,
    setter: ((value: ExtensionInfo[]) => void)|undefined) {
  const base = baseExtensionsMap.get(list) ||
      (Array.isArray(list.extensions) ?
           list.extensions.filter(item => !builtinIds.has(item.id)) :
           []);
  const merged = mergeBuiltins(base, null);
  debugLog('refreshList', {
    baseCount: base.length,
    builtinCount: builtinInfos.length,
    mergedCount: merged.length,
  });
  setListExtensions(list, setter, merged);
}

function observeShadowRoot(root: ShadowRoot) {
  if (shadowObservers.has(root)) {
    return;
  }
  const observer = new MutationObserver(() => scheduleAttachLists());
  shadowObservers.set(root, observer);
  observer.observe(root, {childList: true, subtree: true});
  debugLog('shadow observer added');
}

function collectItemLists(root: ParentNode, out: Set<ItemListElement>) {
  if (!('querySelectorAll' in root)) {
    return;
  }
  const lists = root.querySelectorAll('extensions-item-list');
  for (const list of lists) {
    out.add(list as ItemListElement);
  }
  const elements = root.querySelectorAll('*');
  for (const element of elements) {
    const shadow = (element as HTMLElement).shadowRoot;
    if (shadow) {
      observeShadowRoot(shadow);
      collectItemLists(shadow, out);
    }
  }
}

function attachListsInDocument() {
  const lists = new Set<ItemListElement>();
  collectItemLists(document, lists);
  if (!lists.size) {
    debugLog('attachListsInDocument: none');
    if (attachRetryCount < 30) {
      attachRetryCount += 1;
      setTimeout(() => scheduleAttachLists(), 100);
    }
    return;
  }
  attachRetryCount = 0;
  for (const list of lists) {
    attachList(list, listExtensionsSetter?.bind(list));
  }
}

function scheduleAttachLists() {
  if (attachScheduled) {
    return;
  }
  attachScheduled = true;
  requestAnimationFrame(() => {
    attachScheduled = false;
    attachListsInDocument();
  });
}

function startListObserver() {
  if (listObserver) {
    return;
  }
  const root = document.body || document.documentElement;
  if (!root) {
    debugLog('list observer: no root yet');
    requestAnimationFrame(startListObserver);
    return;
  }
  listObserver = new MutationObserver(() => scheduleAttachLists());
  listObserver.observe(root, {childList: true, subtree: true});
  debugLog('list observer started');
  scheduleAttachLists();
}

function attachList(
    list: ItemListElement,
    setter: ((value: ExtensionInfo[]) => void)|undefined) {
  if (listElements.has(list)) {
    debugLog('attachList skip: already tracked');
    return;
  }
  listElements.add(list);
  if (list.delegate) {
    list.delegate = wrapDelegate(list.delegate);
  }
  debugLog('attachList', {
    hasDelegate: !!list.delegate,
    tag: list.tagName,
  });
  ensureBuiltinsLoaded();
  refreshList(list, setter);
}

function detachList(list: ItemListElement) {
  listElements.delete(list);
  debugLog('detachList');
}

function sanitizeId(name: string): string {
  return `custom_builtin_${name.toLowerCase().replace(/[^a-z0-9]+/g, '_')}`;
}

function createBuiltinInfo(entry: BuiltinExtensionEntry): ExtensionInfo {
  const id = entry.id || sanitizeId(entry.name);
  const displayName = entry.displayName || entry.name;
  const description = entry.description || '';
  const version = entry.version || '';
  const details: BuiltinDetails = {
    ...entry,
    id,
    displayName,
    description,
    version,
  };

  builtinIdToName.set(id, entry.name);
  builtinDetailsById.set(id, details);
  builtinIds.add(id);

  return {
    commands: [],
    isCommandRegistrationHandledExternally: false,
    dependentExtensions: [],
    description,
    disableReasons: {
      suspiciousInstall: false,
      corruptInstall: false,
      updateRequired: false,
      publishedInStoreRequired: false,
      blockedByPolicy: false,
      reloading: false,
      custodianApprovalRequired: false,
      parentDisabledPermissions: false,
      unsupportedManifestVersion: false,
      unsupportedDeveloperExtension: false,
    },
    errorCollection: {isEnabled: false, isActive: false},
    fileAccess: {isEnabled: false, isActive: false},
    fileAccessPendingChange: false,
    homePage: {url: '', specified: false},
    iconUrl: entry.id ? `chrome://extension-icon/${id}/64/0` : '',
    id,
    incognitoAccess: {isEnabled: false, isActive: false},
    userScriptsAccess: {isEnabled: false, isActive: false},
    incognitoAccessPendingChange: false,
    installWarnings: [],
    location: chrome.developerPrivate.Location.INSTALLED_BY_DEFAULT,
    manifestErrors: [],
    manifestHomePageUrl: '',
    mustRemainInstalled: true,
    name: displayName,
    offlineEnabled: false,
    permissions: {simplePermissions: [], canAccessSiteData: false},
    runtimeErrors: [],
    runtimeWarnings: [],
    state: entry.enabled ?
        chrome.developerPrivate.ExtensionState.ENABLED :
        chrome.developerPrivate.ExtensionState.DISABLED,
    type: chrome.developerPrivate.ExtensionType.EXTENSION,
    updateUrl: '',
    userMayModify: true,
    version,
    views: [],
    webStoreUrl: '',
    showSafeBrowsingAllowlistWarning: false,
    showAccessRequestsInToolbar: false,
    safetyCheckWarningReason:
        chrome.developerPrivate.SafetyCheckWarningReason.UNPUBLISHED,
    isAffectedByMV2Deprecation: false,
    didAcknowledgeMV2DeprecationNotice: false,
    canUploadAsAccountExtension: false,
    __customBuiltin: true,
  };
}

async function loadBuiltins(): Promise<void> {
  let entries: BuiltinExtensionEntry[] = [];
  try {
    debugLog('loadBuiltins sendWithPromise');
    const result = await sendWithPromise('getCustomComponentExtensions');
    if (Array.isArray(result)) {
      entries = result as BuiltinExtensionEntry[];
    }
    debugLog('loadBuiltins result', {
      type: typeof result,
      isArray: Array.isArray(result),
      count: Array.isArray(result) ? result.length : 0,
    });
  } catch {
    debugLog('loadBuiltins error');
    entries = [];
  }

  builtinIdToName.clear();
  builtinDetailsById.clear();
  builtinIds.clear();
  builtinInfos = entries.map(entry => createBuiltinInfo(entry));
  debugLog('loadBuiltins done', {
    count: builtinInfos.length,
    ids: builtinInfos.map(info => info.id),
  });
}

function ensureBuiltinsLoaded() {
  if (builtinsPromise) {
    debugLog('ensureBuiltinsLoaded skip: pending');
    return;
  }
  debugLog('ensureBuiltinsLoaded start');
  builtinsPromise = loadBuiltins().finally(() => {
    builtinsPromise = null;
    notifyLists();
  });
}

function notifyLists() {
  debugLog('notifyLists', {count: listElements.size});
  for (const list of listElements) {
    refreshList(list, listExtensionsSetter?.bind(list));
  }
}

function mergeBuiltins(
    base: ExtensionInfo[], filter: Filter|null|undefined): ExtensionInfo[] {
  const applyFilter = (items: ExtensionInfo[]) => filter ?
      items.filter(item => filter(item)) :
      items;
  const builtins = applyFilter(builtinInfos);
  const rest = applyFilter(base).filter(item => !builtinIds.has(item.id));
  return builtins.concat(rest);
}

function ensureBuiltinDetailsStyles() {
  if (document.getElementById('custom-builtins-details-style')) {
    return;
  }
  const style = document.createElement('style');
  style.id = 'custom-builtins-details-style';
  style.textContent = `
#custom-builtins-details-dialog {
  background-color: var(--cr-card-background-color);
  border-radius: var(--cr-card-border-radius);
  box-shadow: var(--cr-card-shadow);
  max-width: 520px;
  padding: 0;
}


#custom-builtins-details-dialog::backdrop {
  background: var(--cr-card-background-color);
}

#custom-builtins-details-dialog .custom-builtins-dialog {
  display: flex;
  flex-direction: column;
  gap: 12px;
  padding: 16px;
}
#custom-builtins-details-dialog .custom-builtins-dialog-title {
  font-size: 16px;
  font-weight: 600;
}
#custom-builtins-details-dialog .custom-builtins-dialog-footer {
  display: flex;
  justify-content: flex-end;
}
#custom-builtins-details-dialog .custom-builtins-dialog-footer button {
  border: 1px solid var(--cr-divider-color, #d0d0d0);
  border-radius: 16px;
  padding: 6px 14px;
  background: var(--cr-card-background-color, #fff);
  color: var(--cr-primary-text-color, #202124);
  cursor: pointer;
}
#custom-builtins-details-dialog .custom-builtins-dialog-footer button:hover {
  background: var(--cr-hover-background-color, #f4f4f4);
}
#custom-builtins-details-dialog .custom-builtins-details {
  display: grid;
  grid-template-columns: max-content 1fr;
  gap: 6px 12px;
}
#custom-builtins-details-dialog .custom-builtins-details dt {
  font-weight: 600;
}
#custom-builtins-details-dialog .custom-builtins-details dd {
  margin: 0;
}
`;
  document.head.appendChild(style);
}

function ensureBuiltinDetailsDialog() {
  if (builtinDetailsDialog && builtinDetailsBody) {
    return;
  }
  ensureBuiltinDetailsStyles();

  const dialog = document.createElement('dialog');
  dialog.id = 'custom-builtins-details-dialog';

  const container = document.createElement('div');
  container.className = 'custom-builtins-dialog';

  const title = document.createElement('div');
  title.className = 'custom-builtins-dialog-title';
  title.textContent = builtinStrings.dialogTitle;

  const body = document.createElement('div');

  const buttonContainer = document.createElement('div');
  buttonContainer.className = 'custom-builtins-dialog-footer';
  const closeButton = document.createElement('button');
  closeButton.type = 'button';
  closeButton.textContent = builtinStrings.closeLabel;
  closeButton.addEventListener('click', () => dialog.close());
  buttonContainer.appendChild(closeButton);

  container.append(title, body, buttonContainer);
  dialog.appendChild(container);
  document.body.appendChild(dialog);

  builtinDetailsDialog = dialog;
  builtinDetailsBody = body;
}

function appendDetailRow(container: HTMLElement, label: string, value: string) {
  const dt = document.createElement('dt');
  dt.textContent = label;
  const dd = document.createElement('dd');
  dd.textContent = value || builtinStrings.valueNa;
  container.append(dt, dd);
}

function updateBuiltinControls(item: HTMLElement & {
  data?: ExtensionInfo;
  shadowRoot: ShadowRoot | null;
}) {
  const shadow = item.shadowRoot;
  if (!shadow) {
    return;
  }

  const toggle = shadow.querySelector<HTMLElement>('#enableToggle');
  const existingButton =
      shadow.querySelector<HTMLElement>('#custom-builtin-install-button');
  if (existingButton) {
    existingButton.remove();
  }
  if (toggle && toggle.dataset['customBuiltinHidden'] !== undefined) {
    toggle.hidden = toggle.dataset['customBuiltinHidden'] === '1';
    delete toggle.dataset['customBuiltinHidden'];
  }
}

function showBuiltinDetails(info: ExtensionInfo) {
  ensureBuiltinDetailsDialog();
  if (!builtinDetailsDialog || !builtinDetailsBody) {
    return;
  }

  const details = builtinDetailsById.get(info.id);
  const name = details?.displayName || info.name || '';
  const description = details?.description || info.description || '';
  const version = details?.version || info.version || '';
  const extensionId = info.id || details?.id || '';
  const enabledByDefault = details?.enabledByDefault ?
      builtinStrings.valueOn :
      builtinStrings.valueOff;
  const enabled =
      info.state === chrome.developerPrivate.ExtensionState.ENABLED ?
          builtinStrings.valueOn :
          builtinStrings.valueOff;
  const hasBackgroundPage = details?.hasBackgroundPage ?
      builtinStrings.valueYes :
      builtinStrings.valueNo;

  builtinDetailsBody.textContent = '';
  const list = document.createElement('dl');
  list.className = 'custom-builtins-details';
  appendDetailRow(list, builtinStrings.nameLabel, name);
  appendDetailRow(list, builtinStrings.descriptionLabel, description);
  appendDetailRow(list, builtinStrings.versionLabel, version);
  appendDetailRow(list, builtinStrings.idLabel, extensionId);
  appendDetailRow(list, builtinStrings.enabledLabel, enabled);
  appendDetailRow(list, builtinStrings.defaultLabel, enabledByDefault);
  appendDetailRow(list, builtinStrings.backgroundPageLabel, hasBackgroundPage);
  builtinDetailsBody.appendChild(list);

  if (typeof builtinDetailsDialog.showModal === 'function') {
    builtinDetailsDialog.showModal();
  } else {
    builtinDetailsDialog.setAttribute('open', '');
  }
}

async function setBuiltinEnabled(id: string, enabled: boolean) {
  const name = builtinIdToName.get(id);
  if (!name) {
    debugLog('setBuiltinEnabled skip: unknown id', {id, enabled});
    return;
  }

  let success = false;
  try {
    debugLog('setBuiltinEnabled sendWithPromise', {id, enabled});
    const result = await sendWithPromise(
        'setCustomComponentExtensionEnabled', name, enabled);
    success = Boolean(result);
  } catch {
    debugLog('setBuiltinEnabled error', {id, enabled});
    success = false;
  }

  debugLog('setBuiltinEnabled done', {id, enabled, success});
  if (success) {
    const nextState = enabled ?
        chrome.developerPrivate.ExtensionState.ENABLED :
        chrome.developerPrivate.ExtensionState.DISABLED;
    builtinInfos = builtinInfos.map(item => item.id === id ?
        {...item, state: nextState} :
        item);
  }
  notifyLists();
}

function wrapDelegate(delegate: any) {
  if (!delegate) {
    return delegate;
  }
  if (delegate.__customBrowserBuiltinsWrapped) {
    debugLog('wrapDelegate skip: already wrapped');
    return delegate;
  }

  return new Proxy(delegate, {
    get(target, prop, receiver) {
      if (prop === '__customBrowserBuiltinsWrapped') {
        return true;
      }
      if (prop === 'setItemEnabled') {
        return (id: string, enabled: boolean) => {
          if (builtinIds.has(id)) {
            ensureBuiltinsLoaded();
            debugLog('delegate setItemEnabled builtin', {id, enabled});
            return setBuiltinEnabled(id, enabled);
          }
          debugLog('delegate setItemEnabled normal', {id, enabled});
          return target.setItemEnabled.call(target, id, enabled);
        };
      }
      if (prop === 'deleteItem') {
        return (id: string) => {
          if (builtinIds.has(id)) {
            ensureBuiltinsLoaded();
            debugLog('delegate deleteItem builtin', {id});
            void setBuiltinEnabled(id, false);
            return;
          }
          debugLog('delegate deleteItem normal', {id});
          return target.deleteItem.call(target, id);
        };
      }
      if (prop === 'uninstallItem') {
        return (id: string) => {
          if (builtinIds.has(id)) {
            ensureBuiltinsLoaded();
            debugLog('delegate uninstallItem builtin', {id});
            return setBuiltinEnabled(id, false);
          }
          debugLog('delegate uninstallItem normal', {id});
          return target.uninstallItem.call(target, id);
        };
      }
      const value = Reflect.get(target, prop, receiver);
      if (typeof value === 'function') {
        return value.bind(target);
      }
      return value;
    },
  });
}

customElements.whenDefined('extensions-item-list').then(() => {
  const ctor = customElements.get('extensions-item-list') as
      (new () => ItemListElement) | undefined;
  if (!ctor) {
    debugLog('extensions-item-list ctor missing');
    return;
  }
  debugLog('extensions-item-list defined');

  const proto = ctor.prototype as ItemListElement & {
    connectedCallback?: () => void;
    disconnectedCallback?: () => void;
    willUpdate?: (changed: Map<PropertyKey, unknown>) => void;
    shouldShowEmptyItemsMessage_?: () => boolean;
    __customBrowserBuiltinsPatched?: boolean;
  };

  if (proto.__customBrowserBuiltinsPatched) {
    debugLog('extensions-item-list already patched');
    return;
  }
  proto.__customBrowserBuiltinsPatched = true;

  const extensionsDescriptor = Object.getOwnPropertyDescriptor(
      proto, 'extensions');
  const extensionsGetter = extensionsDescriptor?.get;
  const extensionsSetter = extensionsDescriptor?.set;
  debugLog('extensions property descriptor', {
    hasDescriptor: !!extensionsDescriptor,
    hasGetter: !!extensionsGetter,
    hasSetter: !!extensionsSetter,
  });
  if (extensionsGetter && extensionsSetter) {
    listExtensionsSetter = extensionsSetter;
    Object.defineProperty(proto, 'extensions', {
      configurable: true,
      get() {
        return extensionsGetter.call(this);
      },
      set(value: ExtensionInfo[]) {
        const list = this as ItemListElement;
        if (!updatingLists.has(list)) {
          baseExtensionsMap.set(
              list, Array.isArray(value) ? value : []);
          debugLog('extensions setter base', {
            count: Array.isArray(value) ? value.length : 0,
          });
        }
        const base = baseExtensionsMap.get(list) || [];
        const merged = mergeBuiltins(base, null);
        setListExtensions(list, extensionsSetter.bind(list), merged);
      },
    });
  } else {
    debugLog('extensions property patch skipped');
  }

  const originalConnected = proto.connectedCallback;
  proto.connectedCallback = function() {
    originalConnected?.call(this);
    attachList(this as ItemListElement, listExtensionsSetter?.bind(this));
  };

  const originalDisconnected = proto.disconnectedCallback;
  proto.disconnectedCallback = function() {
    detachList(this as ItemListElement);
    originalDisconnected?.call(this);
  };

  const originalWillUpdate = proto.willUpdate;
  proto.willUpdate = function(changed: Map<PropertyKey, unknown>) {
    originalWillUpdate?.call(this, changed);
    const list = this as ItemListElement;
    if (changed.has('delegate') && list.delegate) {
      list.delegate = wrapDelegate(list.delegate);
      debugLog('delegate wrapped');
    }
  };

  const originalEmpty = proto.shouldShowEmptyItemsMessage_;
  proto.shouldShowEmptyItemsMessage_ = function() {
    const baseResult = originalEmpty?.call(this) ?? true;
    if (!baseResult) {
      return false;
    }
    return builtinInfos.length === 0;
  };

  startListObserver();
  ensureBuiltinsLoaded();
});

customElements.whenDefined('extensions-item').then(() => {
  const ctor = customElements.get('extensions-item') as
      (new () => HTMLElement) | undefined;
  if (!ctor) {
    debugLog('extensions-item ctor missing');
    return;
  }
  debugLog('extensions-item defined');

  const proto = ctor.prototype as {
    onDetailsClick_?: () => void;
    data?: ExtensionInfo;
    __customBrowserBuiltinsPatched?: boolean;
  };

  if (proto.__customBrowserBuiltinsPatched) {
    debugLog('extensions-item already patched');
    return;
  }
  proto.__customBrowserBuiltinsPatched = true;

  const originalDetails = proto.onDetailsClick_;
  proto.onDetailsClick_ = function() {
    const data = (this as {data?: ExtensionInfo}).data;
    if (data && data.__customBuiltin) {
      debugLog('details click builtin', {id: data.id});
      showBuiltinDetails(data);
      return;
    }
    originalDetails?.call(this);
  };

  const originalUpdated = (proto as {updated?: Function}).updated;
  (proto as {updated?: Function}).updated = function(
      changed: Map<PropertyKey, unknown>) {
    originalUpdated?.call(this, changed);
    updateBuiltinControls(this as HTMLElement & {shadowRoot: ShadowRoot | null});
  };
});

debugLog('builtin_extensions loaded', location.href);
