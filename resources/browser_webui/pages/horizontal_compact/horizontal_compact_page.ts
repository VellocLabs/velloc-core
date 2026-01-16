import {CrLitElement, html, css} from 'chrome://resources/lit/v3_0/lit.rollup.js';
import {installHoverSwap, type HoverSwapHandle} from '../../utils/hover_swap_util.js';
import '../../components/native/native_view_aliases.js';

/**
 * Horizontal Compact Layout (kCompactStandard)
 *
 * Similar to normal Chrome UI but with a collapsible tab strip:
 * - Shows only the toolbar region by default
 * - Has a 10px hover area at the top that reveals the tab strip when hovered
 * - Two native UI regions: TabStripRegionView (top) and ToolbarView (toolbar)
 *
 * When entering this mode, the browser's default layout is restored first
 * (like kStandard mode does) to ensure all native views are in their correct positions.
 */
export class HorizontalCompactPage extends CrLitElement {
  private tabStripHoverSwapHandle_: HoverSwapHandle | null = null;
  private readonly tabStripHoverAttr_ = 'tabstrip-hovered';
  private readonly hoverLeaveDelayMs_ = 300;

  static override styles = css`
    :host {
      display: flex;
      flex-direction: column;
      flex: 1 1 auto;
      min-height: 0;
      width: 100%;
      height: 100%;
      align-items: stretch;
      box-sizing: border-box;
      background: var(--app-bg);
      color: var(--text-primary);
      /* The toolbar area should be draggable for window movement */
      -webkit-app-region: drag;

      /* Tab strip region height when visible */
      --tabstrip-height: 34px;
      /* Hover trigger area height */
      --hover-trigger-height: 10px;
    }

    /* Top container holds the hover trigger and tab strip region */
    #topContainer {
      display: flex;
      flex-direction: column;
      width: 100%;
      position: relative;
      flex: 0 0 auto;
      -webkit-app-region: drag;
    }

    /* Hover trigger area - always visible at top */
    #hoverTrigger {
      flex: 0 0 auto;
      height: var(--hover-trigger-height);
      width: 100%;
      -webkit-app-region: no-drag;
      z-index: 10;
      pointer-events: auto;
    }

    /* Tab strip region - hidden by default, shown on hover */
    #tabStripRegion {
      flex: 0 0 auto;
      height: 0px;
      width: 100%;
      overflow: hidden;
      display: flex;
      align-items: center;
      -webkit-app-region: no-drag;
      transition: height 120ms ease-in-out;
    }

    /* Tab strip region native view holder */
    tab-strip-region-view {
      width: 100%;
      height: var(--tabstrip-height);
      -webkit-app-region: no-drag;
    }

    /* Toolbar region - always visible */
    #toolbarRegion {
      flex: 0 0 auto;
      width: 100%;
      display: flex;
      align-items: center;
      -webkit-app-region: drag;
    }

    toolbar-view {
      width: 100%;
      height: 34px;
      -webkit-app-region: no-drag;
    }

    /* Main content area */
    #mainContents {
      display: flex;
      flex-direction: column;
      flex: 1 1 auto;
      min-height: 0;
      width: 100%;
      height: 100%;
    }

    main-contents-view-no-border {
      flex: 1 1 auto;
      width: 100%;
      -webkit-app-region: no-drag;
    }

    /* When tab strip is being revealed (hover started) */
    :host([tabstrip-hovered]) #tabStripRegion {
      height: var(--tabstrip-height);
      pointer-events: auto;
    }

    /* When tab strip is fully revealed (transition ended) */
    :host([tabstrip-hovered=ended]) #hoverTrigger {
      height: 0px;
    }

    :host([tabstrip-hovered=ended]) tab-strip-region-view {
      display: flex;
    }
  `;

  protected override render() {
    return html`
      <div id="topContainer">
        <div id="hoverTrigger"></div>
        <div id="tabStripRegion">
          <tab-strip-region-viewx></tab-strip-region-viewx>
        </div>
      </div>
      <div id="toolbarRegion">
        <toolbar-view></toolbar-view>
      </div>
      <div id="mainContents">
        <main-contents-view-no-border></main-contents-view-no-border>
      </div>
    `;
  }

  override firstUpdated() {
    this.installHoverSwap_();
  }

  override disconnectedCallback(): void {
    this.destroyHoverSwap_();
    super.disconnectedCallback();
  }

  private installHoverSwap_() {
    const hoverTrigger = this.shadowRoot?.getElementById('hoverTrigger');
    const tabStripRegion = this.shadowRoot?.getElementById('tabStripRegion');
    if (!hoverTrigger || !tabStripRegion) {
      return;
    }

    this.tabStripHoverSwapHandle_?.destroy();
    this.tabStripHoverSwapHandle_ = installHoverSwap({
      enterEl: hoverTrigger,
      targetEl: tabStripRegion,
      hoverEnter: (revealed) => {
        if (revealed) {
          this.setAttribute(this.tabStripHoverAttr_, '');
        } else {
          this.removeAttribute(this.tabStripHoverAttr_);
        }
      },
      transitioned: (propertyName) => {
        // Accept height transitions from tabStripRegion
        if (propertyName && propertyName !== 'height') {
          return false;
        }
        this.setAttribute(this.tabStripHoverAttr_, 'ended');
        return true;
      },
      leaveDelayMs: this.hoverLeaveDelayMs_,
    });
  }

  private destroyHoverSwap_() {
    this.tabStripHoverSwapHandle_?.destroy();
    this.tabStripHoverSwapHandle_ = null;
  }
}

customElements.define('horizontal-compact-page', HorizontalCompactPage);
