// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import type {Tab, TabGroupVisualData} from './browser_controller.mojom-webui.js';
import {BrowserCallbackRouter, PageHandlerFactory, BrowserHandlerRemote} from './browser_controller.mojom-webui.js';
import type {UIMode} from './ui_view_controller.mojom-webui.js';
import {UIViewCallbackRouter, UIViewHandlerRemote} from './ui_view_controller.mojom-webui.js';
import {BrowserCommandID} from './browser_command_ids.mojom-webui.js';
import {IntKey, BoolKey} from './browser_constants.mojom-webui.js';

/**
 * These values are persisted to logs and should not be renumbered or re-used.
 * See tools/metrics/histograms/enums.xml.
 */
export enum CloseTabAction {
  CLOSE_BUTTON = 0,
  SWIPED_TO_CLOSE = 1,
}

export interface BrowserApiProxy {
  activateTab(tabId: number): void;

  /**
   * @return Object of group IDs as strings mapped to their visual data.
   */
  getGroupVisualData(): Promise<{data: {[id: string]: TabGroupVisualData}}>;

  getTabs(): Promise<{tabs: Tab[]}>;

  closeTab(tabId: number, closeTabAction: CloseTabAction): void;

  groupTab(tabId: number, groupId: string): void;

  moveGroup(groupId: string, newIndex: number): void;

  moveTab(tabId: number, newIndex: number): void;

  setThumbnailTracked(tabId: number, thumbnailTracked: boolean): void;

  ungroupTab(tabId: number): void;

  isVisible(): boolean;

  executeBrowserCommand(commandId: BrowserCommandID): void;
  setUIMode(mode: UIMode): void;
  getUIMode(): Promise<{uiMode: UIMode}>;
  getSingleWebTitle(): Promise<{title: string}>;
  openBrowserWindow(): void;
  saveIntPref(key: IntKey, value: number): void;
  saveBoolPref(key: BoolKey, value: boolean): void;
  enableLayoutsSaving(enable: boolean): void;

  /**
   * @return Object with CSS variables as keys and pixel lengths as values
   */
  getLayout(): Promise<{layout: {[key: string]: string}}>;

  showEditDialogForGroup(
      groupId: string, locationX: number, locationY: number, width: number,
      height: number): void;

  showTabContextMenu(tabId: number, locationX: number, locationY: number): void;
  showTabFloatingPreview(tabId: number, locationX: number, locationY: number): void;
  closeTabFloatingPreview(tabId: number): void;

  showBackgroundContextMenu(locationX: number, locationY: number): void;

  closeContainer(): void;

  /** @param durationMs Activation duration time in ms. */
  reportTabActivationDuration(durationMs: number): void;

  /**
   * @param tabCount Number of tabs.
   * @param durationMs Activation duration time in ms.
   */
  reportTabDataReceivedDuration(tabCount: number, durationMs: number): void;

  /**
   * @param tabCount Number of tabs.
   * @param durationMs Creation duration time in ms.
   */
  reportTabCreationDuration(tabCount: number, durationMs: number): void;

  getBrowserCallbackRouter(): BrowserCallbackRouter;

  getNativeViewCallbackRouter(): UIViewCallbackRouter;

  getNativeViewHandler(): UIViewHandlerRemote;
}

export class BrowserApiProxyImpl implements BrowserApiProxy {
  browserCallbackRouter: BrowserCallbackRouter = new BrowserCallbackRouter();
  browserHandler: BrowserHandlerRemote = new BrowserHandlerRemote();

  nativeViewCallbackRouter: UIViewCallbackRouter = new UIViewCallbackRouter();
  nativeViewHandler: UIViewHandlerRemote = new UIViewHandlerRemote();

  constructor() {
    const factory = PageHandlerFactory.getRemote();
    factory.createPageHandler(
        this.browserCallbackRouter.$.bindNewPipeAndPassRemote(),
        this.nativeViewCallbackRouter.$.bindNewPipeAndPassRemote(),
        this.browserHandler.$.bindNewPipeAndPassReceiver(),
        this.nativeViewHandler.$.bindNewPipeAndPassReceiver());
  }

  activateTab(tabId: number) {
    this.browserHandler.activateTab(tabId);
  }

  getGroupVisualData() {
    return this.browserHandler.getGroupVisualData();
  }

  getTabs() {
    return this.browserHandler.getTabs();
  }

  closeTab(tabId: number, closeTabAction: CloseTabAction) {
    this.browserHandler.closeTab(
        tabId, closeTabAction === CloseTabAction.SWIPED_TO_CLOSE);
  }

  groupTab(tabId: number, groupId: string) {
    this.browserHandler.groupTab(tabId, groupId);
  }

  moveGroup(groupId: string, newIndex: number) {
    this.browserHandler.moveGroup(groupId, newIndex);
  }

  moveTab(tabId: number, newIndex: number) {
    this.browserHandler.moveTab(tabId, newIndex);
  }

  setThumbnailTracked(tabId: number, thumbnailTracked: boolean) {
    this.browserHandler.setThumbnailTracked(tabId, thumbnailTracked);
  }

  ungroupTab(tabId: number) {
    this.browserHandler.ungroupTab(tabId);
  }

  isVisible() {
    // TODO(crbug.com/40781526): Move this call out of tabs_api_proxy
    // since it's not related to tabs API.
    return document.visibilityState === 'visible';
  }

  getLayout() {
    return this.browserHandler.getLayout();
  }

  showEditDialogForGroup(
      groupId: string, locationX: number, locationY: number, width: number,
      height: number) {
    this.browserHandler.showEditDialogForGroup(
        groupId, locationX, locationY, width, height);
  }

  showTabContextMenu(tabId: number, locationX: number, locationY: number) {
    this.browserHandler.showTabContextMenu(tabId, locationX, locationY);
  }

  showTabFloatingPreview(tabId: number, locationX: number, locationY: number) {
    this.browserHandler.showTabFloatingPreview(tabId, locationX, locationY);
  }

  closeTabFloatingPreview(tabId: number) {
    this.browserHandler.closeTabFloatingPreview(tabId);
  }

  showBackgroundContextMenu(locationX: number, locationY: number) {
    this.browserHandler.showBackgroundContextMenu(locationX, locationY);
  }

  closeContainer() {
    this.browserHandler.closeContainer();
  }

  reportTabActivationDuration(durationMs: number) {
    this.browserHandler.reportTabActivationDuration(durationMs);
  }

  reportTabDataReceivedDuration(tabCount: number, durationMs: number) {
    this.browserHandler.reportTabDataReceivedDuration(tabCount, durationMs);
  }

  reportTabCreationDuration(tabCount: number, durationMs: number) {
    this.browserHandler.reportTabCreationDuration(tabCount, durationMs);
  }

  executeBrowserCommand(commandId: BrowserCommandID) {
    this.browserHandler.executeBrowserCommand(commandId);
  }

  setUIMode(mode: UIMode) {
    this.nativeViewHandler.setUIMode(mode);
  }

  getUIMode() {
    return this.nativeViewHandler.getUIMode();
  }

  getSingleWebTitle() {
    return this.nativeViewHandler.getSingleWebTitle();
  }

  openBrowserWindow() {
    this.nativeViewHandler.openBrowserWindow();
  }

  saveIntPref(key: IntKey, value: number) {
    this.nativeViewHandler.saveIntPref(key, value);
  }

  saveBoolPref(key: BoolKey, value: boolean) {
    this.nativeViewHandler.saveBoolPref(key, value);
  }

  enableLayoutsSaving(enable: boolean) {
    this.nativeViewHandler.enableLayoutsSaving(enable);
  }

  getBrowserCallbackRouter() {
    return this.browserCallbackRouter;
  }

  getNativeViewCallbackRouter() {
    return this.nativeViewCallbackRouter;
  }

  getNativeViewHandler(): UIViewHandlerRemote {
    return this.nativeViewHandler;
  }

  static getInstance(): BrowserApiProxy {
    return instance || (instance = new BrowserApiProxyImpl());
  }

  static setInstance(obj: BrowserApiProxy) {
    instance = obj;
  }
}

let instance: BrowserApiProxy|null = null;
