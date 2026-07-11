import type {HostedToolExecutionRequest} from './types.js'

export type HostedSkillPreview = {
  id: string
  name: string
  icon?: string
  source: string
  description?: string
}

export type HostedSkill = {
  preview: HostedSkillPreview
  prompt: string
  sourceSkillId?: string
}

export type HostedCreateSkillRequest = {
  prompt: string
  id?: string
  name?: string
  icon?: string
  description?: string
}

export interface HostedToolHost {
  createTask(title?: string): Promise<number>
  resetTask(reason?: string): Promise<void>
  /**
   * Issue a native PerformActions Mojo call. Optional AbortSignal lets the
   * caller impose a JS-side deadline: on signal.abort we detach the pending
   * promise (so resetTask's drain doesn't wait on it) and reject the returned
   * promise with an AbortError. The underlying Mojo call may still be in
   * flight on the native side — callers should fire cancelCurrentActions()
   * separately if they want to try to unblock the actor task queue.
   */
  performActions(
    actions: ArrayBuffer,
    options?: { signal?: AbortSignal },
  ): Promise<ArrayBuffer>
  /**
   * Best-effort cancel of any in-flight PerformActions call on the native
   * side. Optional — hosts that don't expose a cancel Mojo method simply
   * omit this. Callers must race this with a timeout themselves; the host
   * may leave it pending if the native pipe itself is hung.
   *
   * Used by the JS-side observation-timeout guard in toolExecutor: when a
   * post-action full observation doesn't return in time (APC fetch stuck
   * on an error page that never fires FCP, etc.), we cancel the native
   * action and retry with skipAsyncObservation=true so the actor task
   * queue unblocks instead of holding the enqueueForContext chain.
   */
  cancelCurrentActions?(): Promise<void>
  /**
   * Best-effort fetch of lightweight focused-tab metadata (URL + title)
   * WITHOUT going through the actor task queue. Used after an observation
   * timeout so we can tell the model "the page we were on is <X>" even
   * when the APC fetch itself hung (error pages that never fire FCP, etc.).
   * Hosts that can't cheaply fetch this return undefined.
   */
  getFocusedTabMetadata?(): Promise<
    { tab_id?: number; url?: string; title?: string } | undefined
  >
  listSkillPreviews(): Promise<HostedSkillPreview[]>
  getSkill(id: string): Promise<HostedSkill | undefined>
  createSkill(
    request: HostedCreateSkillRequest,
  ): Promise<{ ok: true; modalOpened?: boolean }>
  showManageSkillsUi(): Promise<{ ok: true }>
}

export type HostedToolExecutionHostRequest = HostedToolExecutionRequest
