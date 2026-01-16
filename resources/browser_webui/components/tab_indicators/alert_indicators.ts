// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import './alert_indicator.js';

import {CustomElement} from 'chrome://resources/js/custom_element.js';

import {AlertIndicatorElement} from './alert_indicator.js';
import {TabAlertState} from '../../tabs.mojom-webui.js';

const template: TrustedHTML|string =
    (window as any).trustedTypes?.emptyHTML ?? '';
const styleText = `
  :host {
    height: 100%;
  }

  #container {
    display: grid;
    flex-shrink: 0;
    grid-auto-flow: column;
    grid-gap: 4px;
    height: 100%;
  }

  /* Pinned tabs only show the first indicator. */
  :host-context([pinned]) tabstrip-alert-indicator:not(:first-child) {
    display: none;
  }
`;

export class AlertIndicatorsElement extends CustomElement {
  static override get template() {
    return template;
  }

  private containerEl_: HTMLElement|null = null;
  private alertIndicators_: Map<TabAlertState, AlertIndicatorElement>;

  constructor() {
    super();

    const style = document.createElement('style');
    style.textContent = styleText;
    this.containerEl_ = document.createElement('div');
    this.containerEl_.id = 'container';

    this.shadowRoot?.append(style, this.containerEl_);

    const audioIndicator = new AlertIndicatorElement();
    const recordingIndicator = new AlertIndicatorElement();

    this.alertIndicators_ = new Map([
      [TabAlertState.kMediaRecording, recordingIndicator],
      [TabAlertState.kAudioRecording, recordingIndicator],
      [TabAlertState.kVideoRecording, recordingIndicator],
      [TabAlertState.kTabCapturing, new AlertIndicatorElement()],
      [TabAlertState.kAudioPlaying, audioIndicator],
      [TabAlertState.kAudioMuting, audioIndicator],
      [TabAlertState.kBluetoothConnected, new AlertIndicatorElement()],
      [TabAlertState.kBluetoothScanActive, new AlertIndicatorElement()],
      [TabAlertState.kUsbConnected, new AlertIndicatorElement()],
      [TabAlertState.kHidConnected, new AlertIndicatorElement()],
      [TabAlertState.kSerialConnected, new AlertIndicatorElement()],
      [TabAlertState.kPipPlaying, new AlertIndicatorElement()],
      [TabAlertState.kDesktopCapturing, recordingIndicator],
      [TabAlertState.kVrPresentingInHeadset, new AlertIndicatorElement()],
    ]);
  }

  /**
   * @return A promise that resolves with the number of AlertIndicatorElements
   *     that are currently visible.
   */
  updateAlertStates(alertStates: TabAlertState[]): Promise<number> {
    const container = this.containerEl_ ??
        this.shadowRoot?.querySelector<HTMLElement>('#container') ?? null;

    // Lazily rehydrate if the constructor failed to set the container for any
    // reason (e.g. upgraded after initial render).
    this.containerEl_ = container;
    if (!container) {
      return Promise.resolve(0);
    }

    const alertIndicators =
        alertStates.map(alertState => this.alertIndicators_.get(alertState));

    let alertIndicatorCount = 0;
    for (const [index, alertState] of alertStates.entries()) {
      const alertIndicator = alertIndicators[index];

      // Don't show unsupported indicators.
      if (!alertIndicator) {
        continue;
      }

      // If the same indicator appears earlier in the list of alert indicators,
      // that indicates that there is a higher priority alert state that
      // should display for the shared indicator.
      if (alertIndicators.indexOf(alertIndicator) < index) {
        continue;
      }

      // Always update alert state to ensure the correct icon is displayed.
      alertIndicator.alertState = alertState;

      container.insertBefore(
          alertIndicator, container.children[alertIndicatorCount] as Node);
      // Only fade in if this is just being added to the DOM.
      alertIndicator.show();

      alertIndicatorCount++;
    }

    const animationPromises = Array
                                  .from(
                                      container.children as
                                      HTMLCollectionOf<AlertIndicatorElement>)
                                  .slice(alertIndicatorCount)
                                  .map(indicator => indicator.hide());
    return Promise.all(animationPromises)
        .then(
            () => {
              return container.childElementCount;
            },
            () => {
              // A failure in the animation promises means an animation was
              // canceled and therefore there is a new set of alertStates
              // being animated.
              return -1;
            });
  }
}

declare global {
  interface HTMLElementTagNameMap {
    'tabstrip-alert-indicators': AlertIndicatorsElement;
  }
}

customElements.define('tabstrip-alert-indicators', AlertIndicatorsElement);
