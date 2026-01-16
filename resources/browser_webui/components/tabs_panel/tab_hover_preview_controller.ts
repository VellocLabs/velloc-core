import {BrowserApiProxyImpl} from "../../browser_api_proxy.js";
import type {TabId} from "./types.js";

const DEFAULT_HOVER_DELAY_MS = 400;

class TabHoverPreviewController {
  private hoverDelayMs: number = DEFAULT_HOVER_DELAY_MS;
  private showTimerId: number | null = null;
  private currentTabId: TabId | null = null;
  private previewVisible = false;
  private anchorElement: HTMLElement | null = null;
  private tabElement: HTMLElement | null = null;
  private mouseButtonsPressed = false;

  private readonly onGlobalMouseDown_ = (_event: MouseEvent) => {
    this.mouseButtonsPressed = true;
    this.cancelDueToMouseButtons_();
  };

  private readonly onGlobalMouseUp_ = () => {
    this.mouseButtonsPressed = false;
  };

  private readonly onWindowBlur_ = () => {
    this.mouseButtonsPressed = false;
    this.cancelDueToMouseButtons_();
  };

  constructor() {
    window.addEventListener("mousedown", this.onGlobalMouseDown_, true);
    window.addEventListener("mouseup", this.onGlobalMouseUp_, true);
    window.addEventListener("blur", this.onWindowBlur_);
  }

  setHoverDelay(ms: number) {
    this.hoverDelayMs = ms;
  }

  handleMouseEnter(
      tabId: TabId,
      anchorElement: HTMLElement | null,
      tabElement: HTMLElement | null,
      event?: MouseEvent) {
    if (!tabElement) {
      return;
    }

    if ((event?.buttons ?? 0) !== 0 || this.mouseButtonsPressed) {
      this.cancelDueToMouseButtons_();
      return;
    }

    const effectiveAnchor = anchorElement ?? tabElement;
    this.anchorElement = effectiveAnchor;
    this.tabElement = tabElement;
    if (this.previewVisible && this.currentTabId === tabId) {
      return;
    }
    this.currentTabId = tabId;
    if (this.previewVisible) {
      this.sendShow_(tabId);
      return;
    }

    this.cancelShowTimer_();
    this.showTimerId = window.setTimeout(() => {
      this.showTimerId = null;
      if (this.currentTabId === tabId) {
        this.sendShow_(tabId);
      }
    }, this.hoverDelayMs);
  }

  handleMouseLeave(tabId: TabId) {
    this.cancelShowTimer_();

    if (this.currentTabId !== tabId) {
      return;
    }

    if (!this.previewVisible) {
      this.resetState_();
      return;
    }
    this.sendClose_(tabId);
    this.resetState_();
  }

  handleTabRemoved(tabId: TabId) {
    if (this.currentTabId !== tabId) {
      return;
    }
    this.cancelShowTimer_();
    if (this.previewVisible) {
      this.sendClose_(tabId);
    }
    this.resetState_();
  }

  private cancelShowTimer_() {
    if (this.showTimerId !== null) {
      window.clearTimeout(this.showTimerId);
      this.showTimerId = null;
    }
  }

  private resetState_() {
    this.currentTabId = null;
    this.previewVisible = false;
    this.anchorElement = null;
    this.tabElement = null;
  }

  private sendShow_(tabId: TabId) {
    const anchor = this.anchorElement ?? this.tabElement;
    const tabElement = this.tabElement;
    if (!anchor || !tabElement) {
      return;
    }
    const anchorRect = anchor.getBoundingClientRect();
    const tabRect = tabElement.getBoundingClientRect();
    const locationX = Math.round(anchorRect.right) - 1;
    const locationY = Math.round(tabRect.top) - 4;

    BrowserApiProxyImpl.getInstance().showTabFloatingPreview(
        tabId, locationX, locationY);
    this.previewVisible = true;
  }

  private sendClose_(tabId: TabId) {
    BrowserApiProxyImpl.getInstance().closeTabFloatingPreview(tabId);
    this.previewVisible = false;
  }

  private cancelDueToMouseButtons_() {
    this.cancelShowTimer_();
    if (this.previewVisible && this.currentTabId !== null) {
      this.sendClose_(this.currentTabId);
    }
    this.resetState_();
  }
}

export const tabHoverPreviewController = new TabHoverPreviewController();
