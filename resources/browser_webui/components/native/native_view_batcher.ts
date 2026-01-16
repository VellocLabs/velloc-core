import {NativeViewParams, UIViewHandlerRemote} from '../../ui_view_controller.mojom-webui.js';
import {BrowserApiProxyImpl} from '../../browser_api_proxy.js';

// ---- Batched sender to reduce IPC traffic -----------------------------------
class NativeViewAnchorBatcher {
  private remote: UIViewHandlerRemote = BrowserApiProxyImpl.getInstance().getNativeViewHandler();
  private queue: NativeViewParams[] = [];
  private raf: number | null = null;
  private paused = false;

  setPaused(paused: boolean): void {
    this.paused = paused;
    this.queue = [];
    this.raf = null;
  }

  enqueue(p: NativeViewParams) {
    if (this.paused) return;
    for(const queued of this.queue) {
      if (queued.hashId === p.hashId) {
        const index = this.queue.indexOf(queued);
        this.queue.splice(index, 1);
        break;
      }
    }
    this.queue.push(p);
    if (this.raf == null) this.raf = requestAnimationFrame(() => this.flush());
  }
  private async flush() {
    if (this.paused) return;
    const items = this.queue;
    this.queue = [];
    this.raf = null;
    if (items.length) {
      // Send all layout updates for this frame as a batch.
      await this.remote.upsertNativeViewLayouts(items);
    }
  }
}
export const BATCH = new NativeViewAnchorBatcher();
