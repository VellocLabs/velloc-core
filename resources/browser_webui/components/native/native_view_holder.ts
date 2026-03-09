import {CrLitElement, html, css, type CSSResultGroup} from 'chrome://resources/lit/v3_0/lit.rollup.js';
import {BATCH} from './native_view_batcher.js';
import {ChromeColorID, LayoutAlignment, NativeChildView} from '../../ui_view_controller.mojom-webui.js';

type AlignX = 'left' | 'right' | 'stretch';
type AlignY = 'top' | 'bottom' | 'stretch';

export class NativeViewHolder extends CrLitElement {
  static override styles: CSSResultGroup = css`:host{display:block}`;
  static override properties = {
    viewClass:    {type: String, attribute: 'view-class'},
    viewID:       {type: String, attribute: 'view-id'},
    alignX:       {type: String, attribute: 'align-x'},
    alignY:       {type: String, attribute: 'align-y'},
    // NEW: top-level root selector (preferred)
    rootSelector: {type: String, attribute: 'root-selector'},
    // Back-compat: if you still pass host-selector, we’ll use it when root-selector is absent
    hostSelector: {type: String, attribute: 'host-selector'},
    round:        {type: String, attribute: 'round'},
    hAlign:       {type: Number, attribute: 'h-align'},
    vAlign:       {type: Number, attribute: 'v-align'},
    backgroundColor: {type: String, attribute: 'background-color'},
    extraStyle:   {type: String, attribute: 'extra-style'},
    cornerRadius:   {type: Number, attribute: 'corner-radius'},
  };

  viewClass!: string;
  viewID!: string;
  alignX: AlignX = 'left';
  alignY: AlignY = 'top';
  rootSelector?: string;
  hostSelector?: string; // legacy alias
  round: 'round'|'floor'|'ceil' = 'round';
  hAlign?: LayoutAlignment;
  vAlign?: LayoutAlignment;
  backgroundColor?: string;
  extraStyle?: string;
  cornerRadius?: number;
  childViews?: NativeChildView[];
  gap?: number;
  hashId: number = 0;

  // internals
  private ro?: ResizeObserver;        // self
  private rootRo?: ResizeObserver;    // root
  private mo?: MutationObserver;      // attr changes on self (visibility-affecting)
  private anchorRo?: ResizeObserver;
  private currentAnchorHost?: Element|null;
  private lastJson?: string;
  private currentRoot?: Element|null;
  private lastPresetKey?: string;
  private initialSendRaf_: number|null = null;

  constructor() {
    super();
  }

  override connectedCallback(): void {
    super.connectedCallback();

    this.ro = new ResizeObserver(() => this.scheduleSend());
    this.ro.observe(this);

    // Watch style/class/hidden changes that can affect visibility/layout.
    this.mo = new MutationObserver(() => this.scheduleSend());
    this.mo.observe(this, {attributes: true, attributeFilter: ['style','class','hidden','inert']});

    // Attach root observer
    this.attachRootObserver(this.getLayoutRoot());

    // Apply presets (if any) before first post and layout measurement
    this.maybeApplyPreset();
    // Initial post after layout
    this.initialSendRaf_ = requestAnimationFrame(() => {
      this.initialSendRaf_ = null;
      this.scheduleSend();
    });
  }

  override disconnectedCallback(): void {
    if (this.initialSendRaf_ !== null) {
      cancelAnimationFrame(this.initialSendRaf_);
      this.initialSendRaf_ = null;
    }
    this.ro?.disconnect();
    this.mo?.disconnect();
    this.detachRootObserver();
    super.disconnectedCallback();
  }

  override updated(): void {
    // If root selector/markers changed, rebind to the right root
    const root = this.getLayoutRoot();
    if (root !== this.currentRoot) this.attachRootObserver(root);
    // Re-check presets when inputs change
    this.maybeApplyPreset();
    this.scheduleSend();
  }

  override render() { return html`<slot></slot>`; }


// ---- Root discovery (TOP element) ----

  private getLayoutRoot(): Element {
    if (this.rootSelector) {
      const el = document.querySelector(this.rootSelector);
      if (el) return el;
    }
    if (this.hostSelector) {
      const el = document.querySelector(this.hostSelector);
      if (el) return el;
    }
    // const outermost = this.findOutermostAnchorRoot();
    // if (outermost) return outermost;
    return document.documentElement;
  }

  private findNearestAnchorHost(): Element|null {
    let node: Node|null = this;
    for (let i = 0; i < 256 && node; i++) {
      if (node instanceof Element) {
        if (node.hasAttribute('data-anchor-layout-host')) {
          return node;
        }
        node = node.parentElement ?? node.getRootNode();
      } else if (node instanceof ShadowRoot) {
        node = node.host;
      } else if (node instanceof Document || node instanceof DocumentFragment) {
        node = (node as any).host ?? null;
      } else {
        break;
      }
    }
    return null;
  }

  // private findOutermostAnchorRoot(): Element|null {
  //   let outermost: Element|null = null;
  //   let node: Node|null = this;
  //   for (let i = 0; i < 256 && node; i++) {
  //     if (node instanceof Element) {
  //       if (node.hasAttribute('data-anchor-layout-root')) {
  //         outermost = node;
  //       }
  //       node = node.parentElement ?? node.getRootNode();
  //     } else if (node instanceof ShadowRoot) {
  //       node = node.host;
  //     } else if (node instanceof Document || node instanceof DocumentFragment) {
  //       node = (node as any).host ?? null;
  //     } else {
  //       break;
  //     }
  //   }
  //   return outermost;
  // }

  private attachRootObserver(root: Element) {
    this.detachRootObserver();
    this.currentRoot = root;
    this.rootRo = new ResizeObserver(() => this.scheduleSend());
    this.rootRo.observe(root);

    const anchorHost = this.findNearestAnchorHost();
    if (anchorHost) {
      this.currentAnchorHost = anchorHost;
      this.anchorRo = new ResizeObserver(() => this.scheduleSend());
      this.anchorRo.observe(this.currentAnchorHost);
    }

    // Also listen to scroll on the root's nearest scroll container, since
    // getBoundingClientRect() is viewport-relative and offsets shift on scroll.
    const scroller = this.findNearestScroller(root);
    scroller.addEventListener('scroll', this._onScroll, {passive: true});
    window.addEventListener('resize', this._onResize, {passive: true});
  }

  private detachRootObserver() {
    if (!this.currentRoot) return;
    this.rootRo?.disconnect();
    this.anchorRo?.disconnect();
    this.anchorRo = undefined;
    this.currentAnchorHost = null;
    const scroller = this.findNearestScroller(this.currentRoot);
    scroller.removeEventListener('scroll', this._onScroll);
    window.removeEventListener('resize', this._onResize);
    this.currentRoot = null;
  }

  private _onScroll = () => this.scheduleSend();
  private _onResize = () => this.scheduleSend();

  private findNearestScroller(el: Element): Element|Window {
    // If el itself scrolls, return it; else bubble to window.
    const cs = getComputedStyle(el);
    const overflow = `${cs.overflow}${cs.overflowX}${cs.overflowY}`;
    if (/(auto|scroll)/.test(overflow)) return el;
    return window;
  }

// ---- math/visibility ----
  private roundDIP(v: number) {
    switch (this.round) {
      case 'floor': return Math.floor(v);
      case 'ceil':  return Math.ceil(v);
      default:      return Math.round(v);
    }
  }

  private isActuallyVisible(el: HTMLElement, stopAt: Element): boolean {
    if (!el.isConnected) return false;
      // Fallback: walk up until the root
      let cur: HTMLElement|null = el;
      while (cur && cur !== stopAt) {
        const cs = getComputedStyle(cur);
        if (cs.display === 'none' || cs.visibility === 'hidden' || cs.visibility === 'collapse') {
          return false;
        }
        const rect = cur.getBoundingClientRect();
        if (rect.width === 0 || rect.height === 0) {
          return false;
        }
        cur = cur.parentElement;
      }
    return true;
  }

  // ---- batching / payload ----
  private scheduleSend() {
    if (!this.isConnected) {
      return;
    }
    const payload = this.buildParams();
    const key = JSON.stringify(payload);
    if (key === this.lastJson) return;
    this.lastJson = key;
    BATCH.enqueue(payload);
  }

  private parseBackgroundColor(): number | null {
    if (!this.backgroundColor) {
      return null;
    }
    const token = this.backgroundColor.trim();
    if (!token) {
      return null;
    }
    const numeric = Number(token);
    if (!Number.isNaN(numeric) && (ChromeColorID as any)[numeric] !== undefined) {
      return numeric;
    }
    if ((ChromeColorID as any)[token] !== undefined &&
        typeof (ChromeColorID as any)[token] === 'number') {
      return (ChromeColorID as any)[token] as number;
    }
    console.warn('Unknown ChromeColorID token for native-view-holder', token);
    return null;
  }

  private parseCornerRadius(): number | null {
    if (this.cornerRadius == null) {
      return null;
    }
    const radius = Number(this.cornerRadius);
    if (Number.isNaN(radius)) {
      return null;
    }
    return Math.max(0, Math.round(radius));
  }


  private buildParams() {
    const root = (this.getLayoutRoot() ?? document.documentElement) as HTMLElement;
    const rr = root.getBoundingClientRect();
    const r = this.getBoundingClientRect();

    // Measure relative to the TOP element (root)
    const left   = this.roundDIP(r.left - rr.left);
    const right  = this.roundDIP(rr.right - r.right);   // minus from root's right edge
    const top    = this.roundDIP(r.top - rr.top);
    const bottom = this.roundDIP(rr.bottom - r.bottom); // minus from root's bottom edge
    const width  = this.roundDIP(r.width);
    const height = this.roundDIP(r.height);

    const params: any = {};
    // X axis mapping
    if (this.alignX === 'left') {
      params.left = left; params.width = width;
    } else if (this.alignX === 'right') {
      params.right = right; params.width = width;
    } else { // 'stretch'
      params.left = left; params.right = right; // omit width for native to derive
    }
    // Y axis mapping
    if (this.alignY === 'top') {
      params.top = top; params.height = height;
    } else if (this.alignY === 'bottom') {
      params.bottom = bottom; params.height = height;
    } else { // 'stretch'
      params.top = top; params.bottom = bottom; // omit height for native to derive
    }

    const visible = this.isActuallyVisible(this, root);

    const payload: any = {
      hashId: this.hashId,
      viewClassName: this.viewClass,
      viewId: this.viewID,
      params,
      visible,
    };

    if (this.extraStyle) {
      payload.extraStyle = this.extraStyle;
    }

    const backgroundColorId = this.parseBackgroundColor();
    if (backgroundColorId !== null) {
      payload.backgroundColor = backgroundColorId;
    }

    const radius = this.parseCornerRadius();
    if (radius !== null) {
      payload.radius = radius;
    }

    if (this.hAlign !== undefined) {
      payload.hAlign = this.hAlign;
    }
    if (this.vAlign !== undefined) {
      payload.vAlign = this.vAlign;
    }

    if (this.gap ) {
      payload.gap = this.gap;
    }
    if (this.childViews) {
      payload.childViews = this.childViews.map(child => ({...child}));
    }
    return payload;
  }


  // ---- presets ----
  private maybeApplyPreset() {
    const attrViewClass = this.getAttribute('view-class');
    const attrViewId = this.getAttribute('view-id');
    const lookupViewClass = attrViewClass ?? this.viewClass;
    const lookupViewId = attrViewId ?? this.viewID;
    const key = `${lookupViewId || ''}|${lookupViewClass || ''}`;
    if (key === this.lastPresetKey) return;
    this.lastPresetKey = key;
  }
}

customElements.define('native-view-holder', NativeViewHolder);
