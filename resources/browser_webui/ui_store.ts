import '/strings.m.js';
import {loadTimeData} from 'chrome://resources/js/load_time_data.js';
import {UIMode} from './ui_view_controller.mojom-webui.js';
import {IntKey, BoolKey} from './browser_constants.mojom-webui.js';
import {BrowserApiProxyImpl} from './browser_api_proxy.js';
import {BATCH} from './components/native/native_view_batcher.js';

export interface UiStoreObserver {
  onUiModeChanged(uiMode: UIMode): void;
}

// Central UI store for WebUI-level state (starting with uiMode and layout).
// Owns URL synchronization via CrRouter so components don't each talk to it.
export class UiStore {
  private static instance: UiStore;

  private mode_: UIMode = UIMode.kSidebar;
  private leftPaneWidth_: null | number = null;
  private leftPaneLocked_ = false;
  private readonly observers_ = new Set<UiStoreObserver>();

  private constructor() {
    const modeKey = IntKey[IntKey.kUIMode];
    if (loadTimeData.valueExists(modeKey)) {
      const stored = loadTimeData.getInteger(modeKey);
      // A per-window mode (kNexus, kSingleWeb) is one window's alone and only
      // native says which window: drawn from the shared baked value, a browser
      // window would mount the Nexus panel and its guest until native answered.
      if ((UIMode as any)[stored] !== undefined && stored !== UIMode.kNexus &&
          stored !== UIMode.kSingleWeb) {
        this.mode_ = stored as unknown as UIMode;
      }
    }

    const leftPaneWidthKey = IntKey[IntKey.kLeftPanelWidth];
    if (loadTimeData.valueExists(leftPaneWidthKey)) {
      const stored = loadTimeData.getInteger(leftPaneWidthKey);
      if (stored > 0) {
        this.leftPaneWidth_ = stored;
      }
    }

    // IntKey.kNexusViewWidth is no longer read: the kNexus chat/browser
    // split moved into the React guest (localStorage
    // 'shift-webview-area-width'); the shell-side <cr-splitter> it sized
    // is gone. The enum entry stays reserved so stale persisted ints are
    // simply ignored.

    const leftPaneLockedKey = BoolKey[BoolKey.kLeftPanelLocked];
    if (loadTimeData.valueExists(leftPaneLockedKey)) {
      const stored = loadTimeData.getBoolean(leftPaneLockedKey);
      if (stored) {
        this.leftPaneLocked_ = stored;
      }
    }

    const router =
        BrowserApiProxyImpl.getInstance().getNativeViewCallbackRouter();
    router.prefUIModeChanged.addListener(
        (uiMode: UIMode) => this.setMode_(uiMode, /*syncToNative=*/true));

    BATCH.setPaused(this.mode_ === UIMode.kStandard);

    // The shell data source only bakes the profile-global mode, so the value
    // read above can be wrong for a per-window mode (e.g. the dedicated kNexus
    // window would otherwise render the last-saved browser mode like kSidebar).
    // Pull this window's authoritative mode from native and apply it. Unlike
    // the startup push, a request/response cannot be lost to page-load timing.
    BrowserApiProxyImpl.getInstance().getUIMode().then(({uiMode}) => {
      if ((UIMode as any)[uiMode] !== undefined) {
        this.setMode_(uiMode, /*syncToNative=*/false);
      }
    });
  }

  static getInstance(): UiStore {
    return this.instance || (this.instance = new UiStore());
  }

  get mode(): UIMode {
    return this.mode_;
  }

  get leftPaneWidth(): number | null {
    return this.leftPaneWidth_;
  }

  get leftPaneLocked(): boolean {
    return this.leftPaneLocked_;
  }

  addObserver(observer: UiStoreObserver) {
    this.observers_.add(observer);
    this.syncObserver_(observer);
  }

  removeObserver(observer: UiStoreObserver) {
    this.observers_.delete(observer);
  }

  setLeftPaneWidth(width: number) {
    const roundedWidth = Math.round(width);
    if (!Number.isFinite(roundedWidth) || roundedWidth <= 0 ||
        this.leftPaneWidth_ === roundedWidth) {
      return;
    }
    this.leftPaneWidth_ = roundedWidth;
    BrowserApiProxyImpl.getInstance().saveIntPref(
        IntKey.kLeftPanelWidth, this.leftPaneWidth_);
  }

  setLeftPaneLocked(locked: boolean) {
    if (this.leftPaneLocked_ === locked) {
      return;
    }
    this.leftPaneLocked_ = locked;
    BrowserApiProxyImpl.getInstance().saveBoolPref(
        BoolKey.kLeftPanelLocked, this.leftPaneLocked_);
  }

  private syncObserver_(observer: UiStoreObserver) {
    observer.onUiModeChanged(this.mode_);
  }

  private notifyUiModeChanged_() {
    if (!this.observers_.size) {
      return;
    }
    for (const observer of this.observers_) {
      observer.onUiModeChanged(this.mode_);
    }
  }

  setMode(mode: UIMode): void {
    this.setMode_(mode, /*syncToNative=*/true);
  }

  private setMode_(mode: UIMode, syncToNative = true) {
    if (this.mode_ === mode) {
      return;
    }
    this.mode_ = mode;
    if (syncToNative) {
      BrowserApiProxyImpl.getInstance().setUIMode(mode);
    }
    BATCH.setPaused(mode === UIMode.kStandard);
    this.notifyUiModeChanged_();
  }
}

export const uiStore = UiStore.getInstance();
