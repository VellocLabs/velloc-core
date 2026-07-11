/*
Settings patch loader: dynamically loads per-page patch modules.
*/

interface PatchModule {
  attach: () => void;
  detach?: () => void;
}

interface PatchEntry {
  matchPath: (path: string) => boolean;
  load: () => Promise<PatchModule>;
  active: boolean;
  module?: PatchModule;
  loadPromise?: Promise<PatchModule>;
}

const GET_STARTED_PATH = '/getStarted';
const initialPatchModules = new Map<string, string>([
  [GET_STARTED_PATH, 'get_started_patch.js'],
]);
const windowWithInitialPath = window as Window & {
  customBrowserSettingsInitialPath?: string;
};
if (!windowWithInitialPath.customBrowserSettingsInitialPath) {
  windowWithInitialPath.customBrowserSettingsInitialPath =
      window.location.pathname;
}
const initialPath = windowWithInitialPath.customBrowserSettingsInitialPath;
let blockedInitialReplace = false;
const originalReplaceState = history.replaceState.bind(history);
history.replaceState =
    ((...args: Parameters<History['replaceState']>) => {
      const url = args[2];
      if (!blockedInitialReplace && initialPatchModules.has(initialPath) &&
          window.location.pathname === initialPath && url === '/') {
        blockedInitialReplace = true;
        return;
      }
      return originalReplaceState(...args);
    }) as History['replaceState'];

const patchEntries: PatchEntry[] = [
  {
    matchPath: () => true,
    load: () => loadPatchModule('get_started_patch.js'),
    active: false,
  },
  {
    matchPath: (path: string) => path === '/help' || path.startsWith('/help/'),
    load: () => loadPatchModule('about_patch.js'),
    active: false,
  },
  {
    matchPath: (path: string) =>
        path === '/appearance' || path.startsWith('/appearance/'),
    load: () => loadPatchModule('appearance_patch.js'),
    active: false,
  },
];

let updateScheduled = false;
let started = false;

function loadPatchModule(fileName: string): Promise<PatchModule> {
  return import(`./${fileName}`) as Promise<PatchModule>;
}

function loadInitialPatchImmediatelyIfNeeded() {
  const fileName = initialPatchModules.get(window.location.pathname);
  if (!fileName) {
    return;
  }
  loadPatchModule(fileName).then(module => {
    module.attach();
  });
}

function ensureLoaded(entry: PatchEntry): Promise<PatchModule> {
  if (entry.loadPromise) {
    return entry.loadPromise;
  }
  entry.loadPromise = entry.load().then((module) => {
    entry.module = module;
    return module;
  });
  return entry.loadPromise;
}

function updateForPath(path: string) {
  for (const entry of patchEntries) {
    const shouldActivate = entry.matchPath(path);
    if (shouldActivate) {
      ensureLoaded(entry).then((module) => {
        if (!entry.matchPath(window.location.pathname)) {
          return;
        }
        if (!entry.active) {
          entry.active = true;
          module.attach();
        }
      });
      continue;
    }

    if (entry.active) {
      entry.active = false;
      entry.module?.detach?.();
    }
  }
}

function scheduleUpdate() {
  if (updateScheduled) {
    return;
  }
  updateScheduled = true;
  requestAnimationFrame(() => {
    updateScheduled = false;
    updateForPath(window.location.pathname);
  });
}

function startObserver() {
  if (started) {
    return;
  }
  if (!document.querySelector('settings-ui')) {
    requestAnimationFrame(startObserver);
    return;
  }
  started = true;

  const originalPushState = history.pushState.bind(history);
  history.pushState =
      ((...args: Parameters<History['pushState']>) => {
        const result = originalPushState(...args);
        scheduleUpdate();
        return result;
      }) as History['pushState'];

  const originalReplaceState = history.replaceState.bind(history);
  history.replaceState =
      ((...args: Parameters<History['replaceState']>) => {
        const result = originalReplaceState(...args);
        scheduleUpdate();
        return result;
      }) as History['replaceState'];

  window.addEventListener('popstate', scheduleUpdate);
  scheduleUpdate();
}

customElements.whenDefined('settings-ui')
    .then(() => requestAnimationFrame(startObserver));
requestAnimationFrame(startObserver);
loadInitialPatchImmediatelyIfNeeded();
