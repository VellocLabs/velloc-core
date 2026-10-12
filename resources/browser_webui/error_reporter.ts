/**
 * GlobalErrorReporter: captures top-level WebUI JS errors, resource load
 * failures and unhandled rejections; no iframe forwarding; logs to console.
 */
export interface ErrorPayload {
  type: 'error' | 'resource-error' | 'unhandledrejection';
  message?: string;
  name?: string;
  stack?: string;
  fileName?: string;
  line?: number;
  column?: number;
  resource?: { tag: string; src?: string; href?: string };
  href: string;
  userAgent: string;
  timestamp: number;
  extra?: Record<string, unknown>;
}

export type ErrorSink = (payload: ErrorPayload) => void | Promise<void>;

export interface ErrorReporterOptions {
  sink?: ErrorSink;               // where to send errors (defaults to console.log)
  appInfo?: Record<string, any>;  // static info to attach (version, channel, etc.)
}

export class GlobalErrorReporter {
  private sink: ErrorSink;
  private unsub: Array<() => void> = [];
  private appInfo?: Record<string, any>;

  constructor(opts: ErrorReporterOptions = {}) {
    this.sink = opts.sink ?? ((p) => console.log('[WebUI Error]', p));
    this.appInfo = opts.appInfo;
  }

  /** Start listening globally. Call once at app bootstrap. */
  start(): void {
    // 1) JS runtime errors (including those thrown in event handlers)
    const onError = (ev: ErrorEvent) => {
      const payload: ErrorPayload = {
        type: 'error',
        message: ev.message,
        name: ev.error?.name,
        stack: ev.error?.stack,
        fileName: ev.filename,
        line: ev.lineno,
        column: ev.colno,
        href: location.href,
        userAgent: navigator.userAgent,
        timestamp: Date.now(),
        extra: this.appInfo,
      };
      this.safeSend(payload);
    };
    window.addEventListener('error', onError);
    this.unsub.push(() => window.removeEventListener('error', onError));

    // 2) Resource loading errors (img/script/link). These do NOT bubble; capture phase
    const onResourceError = (ev: Event) => {
      const t = ev.target as (HTMLElement & { src?: string; href?: string }) | null;
      if (!t) return;
      const tag = t.tagName?.toLowerCase?.() ?? 'unknown';
      if (tag === 'img' || tag === 'script' || tag === 'link') {
        const payload: ErrorPayload = {
          type: 'resource-error',
          resource: { tag, src: (t as any).src, href: (t as any).href },
          href: location.href,
          userAgent: navigator.userAgent,
          timestamp: Date.now(),
          extra: this.appInfo,
        };
        this.safeSend(payload);
      }
    };
    window.addEventListener('error', onResourceError, true);
    this.unsub.push(() => window.removeEventListener('error', onResourceError, true as any));

    // 3) Unhandled promise rejections
    const onRejection = (ev: PromiseRejectionEvent) => {
      const reason = ev.reason;
      const payload: ErrorPayload = {
        type: 'unhandledrejection',
        message: (typeof reason === 'string') ? reason : reason?.message,
        name: reason?.name,
        stack: reason?.stack,
        href: location.href,
        userAgent: navigator.userAgent,
        timestamp: Date.now(),
        extra: this.appInfo,
      };
      this.safeSend(payload);
    };
    window.addEventListener('unhandledrejection', onRejection);
    this.unsub.push(() => window.removeEventListener('unhandledrejection', onRejection));
  }

  /** Stop listening and remove all handlers. */
  stop(): void { this.unsub.splice(0).forEach((fn) => fn()); }

  /** Send to sink with try/catch to avoid recursive error storms. */
  private safeSend(payload: ErrorPayload): void {
    try { this.sink(payload); } catch (e) { console.log('Error sink failed', e, payload); }
  }
}

// Singleton
export const errorReporter = new GlobalErrorReporter();
