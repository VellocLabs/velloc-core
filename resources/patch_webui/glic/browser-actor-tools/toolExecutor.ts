import {
  buildActionsProto,
  buildObserveProto,
  isSimpleToolCallError,
  parseActionsResult,
  parseActionsResultWithObservations,
  parseSimpleToolCall,
} from './actionsConverter.js'
import type { ElementRegistry } from './elementRegistry.js'
import {
  buildParentIndex,
  collectElementText,
  findEnclosingElementIndex,
  isActionable,
  joinPageVisibleText,
  toSimpleObservation,
} from './observationParser.js'
import {
  isBrowserActorTool,
} from './toolDefinitions.js'
import type {
  HostedCreateSkillRequest,
  HostedToolHost,
} from './hostTypes.js'
import type {
  BrowserAction,
  BrowserActResult,
  BrowserObserveResult,
  FindTextMatch,
  HostedToolExecutionRequest,
  ObservationConfig,
  PageElement,
  PageObservation,
  SimpleObservation,
  SimpleTabInfo,
  SimpleToolResult,
  WindowInfo,
} from './types.js'
import {OBSERVATION_PRESETS} from './types.js'

type BrowserActionWithTabId = Exclude<BrowserAction, {
  type: 'wait';
}|{
  type: 'create_tab';
}>

const SKILL_SOURCE_NAMES: Record<number, string> = {
  0: 'unknown',
  1: 'first_party',
  2: 'user_created',
  3: 'derived_from_first_party',
}

const RECOVERABLE_TASK_RESULT_NAMES = new Set([
  'TASK_PAUSED',
  'TASK_WENT_AWAY',
  'EXECUTOR_DESTROYED',
])

// Timeout result names produced by performActionsWithDeadline when our
// JS-side deadline fires. Distinct from RECOVERABLE: we reset the task to
// unblock the *next* tool call (the zombie native BuildActionsResultWithObservations
// still holds the single callback slot on the original ActorTask), but we do
// NOT retry the current action — the renderer may have actually completed it
// despite our cancel, and a silent second click/type/navigate is worse than
// reporting a clean timeout to the model.
const TIMEOUT_RESULT_NAMES = new Set([
  'ACTION_TIMEOUT',
  'STATUS_TIMEOUT',
  'OBSERVATION_TIMEOUT',
])

// Busy-executor errors: the Chromium executor is still processing a previous
// action's async work (APC snapshot, navigation commit). These are transient
// and resolve by waiting — the task itself is valid and should NOT be reset.
const BUSY_EXECUTOR_RESULT_NAMES = new Set([
  'EXECUTOR_BUSY',
  'EXECUTION_ENGINE_EXISTING_ACTION',
])

function normalizeArgs(value: unknown): Record<string, unknown> {
  if (!value || typeof value !== 'object' || Array.isArray(value)) {
    return {}
  }
  return value as Record<string, unknown>
}

function toOwnedArrayBuffer(bytes: Uint8Array): ArrayBuffer {
  const copy = new Uint8Array(bytes.byteLength)
  copy.set(bytes)
  return copy.buffer
}

function isPositiveInteger(value: unknown): value is number {
  return typeof value === 'number' && Number.isInteger(value) && value > 0
}

function isNonEmptyString(value: unknown): value is string {
  return typeof value === 'string' && value.trim().length > 0
}

function isRecoverableTaskResultName(value: string): boolean {
  return RECOVERABLE_TASK_RESULT_NAMES.has(value)
}

function isBusyExecutorResultName(value: string): boolean {
  return BUSY_EXECUTOR_RESULT_NAMES.has(value)
}

function isTimeoutResultName(value: string): boolean {
  return TIMEOUT_RESULT_NAMES.has(value)
}

async function executeWithTaskRecovery<T extends {
  action_result_name: string;
}>(host: HostedToolHost, run: (taskId: number) => Promise<T>): Promise<T> {
  let taskId = await host.createTask()
  let result = await run(taskId)

  // Busy-executor retry: the Chromium executor is still processing a previous
  // action's async work. Wait briefly and retry with the same task — the
  // executor will finish. Two retries with increasing wait: 500ms covers the
  // common case (APC observation completing), 1500ms covers navigations that
  // haven't committed yet.
  if (isBusyExecutorResultName(result.action_result_name)) {
    await new Promise(resolve => setTimeout(resolve, 500))
    result = await run(taskId)
    if (isBusyExecutorResultName(result.action_result_name)) {
      await new Promise(resolve => setTimeout(resolve, 1500))
      result = await run(taskId)
    }
  }

  // Reset-only on our JS-side timeouts. The native task is presumed stuck
  // (its single callback slot is held by an observation-build we abandoned),
  // so the NEXT tool call must not reuse this taskId — otherwise it hits
  // EXECUTION_ENGINE_EXISTING_ACTION forever. We do NOT retry the current
  // action on a fresh task: the renderer may have already completed it, and
  // a double click/type/navigate is worse than reporting a clean timeout.
  if (isTimeoutResultName(result.action_result_name)) {
    logWarn('executeWithTaskRecovery:timeout_reset', {
      resultName: result.action_result_name,
      taskId,
    })
    try {
      await host.resetTask(`timeout_reset:${result.action_result_name}`)
    } catch (err) {
      logError('executeWithTaskRecovery:timeout_reset_failed', {
        resultName: result.action_result_name,
        error: err instanceof Error ? err.message : String(err),
      })
    }
    return result
  }

  if (!isRecoverableTaskResultName(result.action_result_name)) {
    return result
  }

  await host.resetTask(`recover:${result.action_result_name}`)
  taskId = await host.createTask()
  result = await run(taskId)
  if (isRecoverableTaskResultName(result.action_result_name)) {
    await host.resetTask(`recover_failed:${result.action_result_name}`)
  }
  return result
}

// Per-call deadlines for host.performActions. Every Mojo call gets one —
// the native side has multiple hang sites (action phase waiting for an
// iframe popup that never responds; observation phase's APC fetch stuck
// on an error page with no FCP) and upstream Chromium has no timeouts on
// either. If we don't bound every call individually, one hung
// executeHostedTool invocation blocks enqueueForContext for the whole
// session until the bridge's 30s synthesized-timeout fires.
//
// Values are tuned under the bridge transport's 30s ceiling so a timeout
// here returns a usable error to the guest instead of a bridge_timeout.
const ACTION_PERFORM_TIMEOUT_MS = 12000
const STATUS_PERFORM_TIMEOUT_MS = 6000
const OBSERVATION_PERFORM_TIMEOUT_MS = 8000
const CANCEL_FIRE_AND_FORGET = true

// -------- Diagnostic logging --------------------------------------------
// Lightweight structured logs so we can tell which pipeline stage blocked
// in the renderer console. Every log line carries BOTH [NexusTrace][Host]
// and [browser-actor] prefixes, so whether you filter on the existing
// NexusTrace stream (emitted by nexusTrace.ts — uses console.info) or on a
// new browser-actor filter, you catch them all. The op/details payload is
// identical in both cases.
// Set LOG_VERBOSE=false to silence lower-priority lines if they get noisy
// — timeouts/errors always log regardless.
const LOG_VERBOSE = true
const LOG_PREFIX_NEXUS = '[NexusTrace][Host]'
const LOG_PREFIX_ACTOR = '[browser-actor]'

function logInfo(op: string, details?: Record<string, unknown>): void {
  if (!LOG_VERBOSE) return
  try {
    // console.info matches what nexusTrace uses — same visibility/filter.
    // eslint-disable-next-line no-console
    console.info(LOG_PREFIX_NEXUS, LOG_PREFIX_ACTOR, op, details ?? '')
  } catch {
    // console may be unavailable in some embedder contexts — swallow.
  }
}

function logWarn(op: string, details?: Record<string, unknown>): void {
  try {
    // eslint-disable-next-line no-console
    console.warn(LOG_PREFIX_NEXUS, LOG_PREFIX_ACTOR, op, details ?? '')
  } catch {
    /* noop */
  }
}

function logError(op: string, details?: Record<string, unknown>): void {
  try {
    // eslint-disable-next-line no-console
    console.error(LOG_PREFIX_NEXUS, LOG_PREFIX_ACTOR, op, details ?? '')
  } catch {
    /* noop */
  }
}

let nextCallSeq = 1
function allocCallId(): number {
  return nextCallSeq++
}

// Errors from the signal-abort path are thrown instances of Error; we tag
// them via this marker class so call sites can distinguish "JS-side gave
// up" from a real native error without string matching.
class PerformActionsAbortedError extends Error {
  constructor(reason: string) {
    super(`performActions aborted: ${reason}`)
    this.name = 'PerformActionsAbortedError'
  }
}

function isPerformActionsAborted(error: unknown): boolean {
  return error instanceof PerformActionsAbortedError ||
      (error instanceof Error && error.name === 'AbortError')
}

// Run one host.performActions with a JS-side deadline. On timeout:
//   (1) abort the caller-visible promise (detaches pendingActions),
//   (2) fire glicBrowserCancelActions as a best-effort unblock of the
//       native actor task — useful when the hang is in the Act phase so
//       the next tool's performActions isn't rejected with BUSY,
//   (3) return undefined so the caller can produce a meaningful error.
// The caller is expected to NOT retry blindly — further retries would
// just queue behind the still-stuck native call.
async function performActionsWithDeadline(
    host: HostedToolHost,
    proto: ArrayBuffer,
    deadlineMs: number,
    tag: string): Promise<ArrayBuffer|undefined> {
  const callId = allocCallId()
  const startedAt = Date.now()
  const controller = new AbortController()
  logInfo('performActions:start', {
    callId,
    tag,
    deadlineMs,
    protoBytes: proto.byteLength,
  })
  const timer = setTimeout(() => {
    logWarn('performActions:deadline_fired', {
      callId,
      tag,
      elapsedMs: Date.now() - startedAt,
      deadlineMs,
    })
    controller.abort(new PerformActionsAbortedError(`deadline_${deadlineMs}ms`))
  }, deadlineMs)
  try {
    const buffer = await host.performActions(proto, {signal: controller.signal})
    logInfo('performActions:ok', {
      callId,
      tag,
      elapsedMs: Date.now() - startedAt,
      resultBytes: buffer.byteLength,
    })
    return buffer
  } catch (error: unknown) {
    if (isPerformActionsAborted(error)) {
      logWarn('performActions:aborted', {
        callId,
        tag,
        elapsedMs: Date.now() - startedAt,
      })
      if (CANCEL_FIRE_AND_FORGET &&
          typeof host.cancelCurrentActions === 'function') {
        logInfo('performActions:fire_cancel', {callId, tag})
        void host.cancelCurrentActions().catch(cancelErr => {
          logWarn('performActions:fire_cancel_failed', {
            callId,
            tag,
            error: String(cancelErr),
          })
        })
      }
      return undefined
    }
    logError('performActions:error', {
      callId,
      tag,
      elapsedMs: Date.now() - startedAt,
      error: error instanceof Error ? error.message : String(error),
    })
    throw error
  } finally {
    clearTimeout(timer)
  }
}

function buildBusyTimeoutResult(
    action_result_name: string): BrowserActResult {
  return {
    success: false,
    action_result_code: -1,
    action_result_name,
    failed_action_index: null,
    tabs: [],
    windows: [],
    script_results: [],
  }
}

function buildEmptyObserveResult(
    action_result_name: string): BrowserObserveResult {
  return {
    success: false,
    action_result_code: -1,
    action_result_name,
    windows: [],
    observations: [],
  }
}

async function executeBrowserActions(
    host: HostedToolHost,
    actions: BrowserAction[]): Promise<BrowserActResult> {
  const actionTypes = actions.map(a => a.type).join(',')
  logInfo('executeBrowserActions:begin', {
    actionTypes,
    actionCount: actions.length,
  })
  const out = await executeWithTaskRecovery(host, async taskId => {
    const proto = buildActionsProto(taskId, actions, {
      skipObservation: true,
    })
    const buffer = await performActionsWithDeadline(
        host, toOwnedArrayBuffer(proto), ACTION_PERFORM_TIMEOUT_MS,
        `action:${actionTypes}`)
    if (buffer === undefined) {
      logWarn('executeBrowserActions:timeout', {actionTypes})
      return buildBusyTimeoutResult('ACTION_TIMEOUT')
    }
    return parseActionsResult(buffer)
  })
  logInfo('executeBrowserActions:end', {
    actionTypes,
    success: out.success,
    resultName: out.action_result_name,
    failedIndex: out.failed_action_index,
  })
  return out
}

async function observeBrowserStatus(
    host: HostedToolHost): Promise<BrowserActResult> {
  logInfo('observeBrowserStatus:begin', {})
  const out = await executeWithTaskRecovery(host, async taskId => {
    const proto = buildObserveProto(taskId, {
      config: {
        skipAsyncObservation: true,
      },
    })
    const buffer = await performActionsWithDeadline(
        host, toOwnedArrayBuffer(proto), STATUS_PERFORM_TIMEOUT_MS, 'status')
    if (buffer === undefined) {
      logWarn('observeBrowserStatus:timeout', {})
      return buildBusyTimeoutResult('STATUS_TIMEOUT')
    }
    return parseActionsResult(buffer)
  })
  logInfo('observeBrowserStatus:end', {
    success: out.success,
    resultName: out.action_result_name,
    windowCount: out.windows.length,
  })
  return out
}

async function executeObservation(
    host: HostedToolHost, observationConfig: ObservationConfig,
    observeTabId?: number): Promise<BrowserObserveResult> {
  const isSkipAsync = observationConfig.skipAsyncObservation === true
  logInfo('executeObservation:begin', {
    observeTabId,
    skipAsync: isSkipAsync,
  })
  const out = await executeWithTaskRecovery(host, async taskId => {
    const buffer = await performActionsWithDeadline(
        host,
        toOwnedArrayBuffer(buildObserveProto(taskId, {
          observeTabId,
          config: observationConfig,
        })),
        OBSERVATION_PERFORM_TIMEOUT_MS,
        isSkipAsync ? 'observe:skipAsync' : 'observe:full')
    if (buffer === undefined) {
      logWarn('executeObservation:timeout', {observeTabId, skipAsync: isSkipAsync})
      return buildEmptyObserveResult('OBSERVATION_TIMEOUT')
    }
    return parseActionsResultWithObservations(buffer, observationConfig)
  })
  logInfo('executeObservation:end', {
    observeTabId,
    success: out.success,
    resultName: out.action_result_name,
    observationCount: out.observations.length,
    elementCount: out.observations[0]?.elements.length ?? 0,
  })
  return out
}

function observeResultTimedOut(result: BrowserObserveResult): boolean {
  return result.action_result_name === 'OBSERVATION_TIMEOUT' &&
      result.action_result_code === -1
}

// ========================================================================
//  Observation timeout guard
// ========================================================================
//
// The native PerformActions observation path fetches an AnnotatedPageContent
// (APC) snapshot for the observed tab, and there is no timeout on the APC
// fetch upstream. On an error page that never fires FCP (the
// `waiting_for_fcp_` latch in AnnotatePageContentRequest never clears), the
// fetch hangs indefinitely. Since `enqueueForContext` serializes all tool
// dispatches per sessionId, one hung PerformActions blocks every subsequent
// tool call until the guest bridge's 30s timeout synthesizes a fake
// response — by which point the user has watched a chain of fake
// bridge_timeout errors and given up.
//
// Fix: on the JS side we race every full observation against a short
// deadline. If the native call doesn't come back in time we:
//   (1) fire glicBrowserCancelActions so the native side unblocks,
//   (2) retry with skipAsyncObservation=true — the proto flag that makes
//       actor_proto_conversion short-circuit the APC fetch
//       (actor_proto_conversion.cc:1202-1207). The retry returns with just
//       the tab_id populated on the tab observation, which is enough for
//       downstream tabs/IDs plumbing,
//   (3) best-effort fetch focused-tab URL/title via the separate
//       glicBrowserGetContextFromFocusedTab pipe (not routed through the
//       actor task queue), so we can surface an informative message to the
//       model explaining what page it was trying to observe.
//
// The skipAsync retry is safe because the action itself has already
// succeeded by this point — we only lost the observation payload, not the
// click/navigate/type effect.
const OBSERVATION_TIMEOUT_MS = 8000
const CANCEL_TIMEOUT_MS = 3000
const METADATA_TIMEOUT_MS = 2000

const TIMEOUT_SENTINEL = Symbol('observation_timeout')

function waitWithTimeout<T>(
    promise: Promise<T>, timeoutMs: number): Promise<T|typeof TIMEOUT_SENTINEL> {
  let handle: ReturnType<typeof setTimeout>|undefined
  const timer = new Promise<typeof TIMEOUT_SENTINEL>(resolve => {
    handle = setTimeout(() => resolve(TIMEOUT_SENTINEL), timeoutMs)
  })
  return Promise.race([promise, timer]).finally(() => {
    if (handle !== undefined) clearTimeout(handle)
  })
}

export type ObservationTimeoutInfo = {
  tab_id?: number
  url?: string
  title?: string
}

async function executeObservationWithTimeout(
    host: HostedToolHost, observationConfig: ObservationConfig,
    observeTabId?: number): Promise<{
      result: BrowserObserveResult;
      timedOut: boolean;
      timeoutInfo?: ObservationTimeoutInfo;
    }> {
  const startedAt = Date.now()
  logInfo('observationWithTimeout:begin', {
    observeTabId,
    outerTimeoutMs: OBSERVATION_TIMEOUT_MS,
  })
  const primary = executeObservation(host, observationConfig, observeTabId)
  // Make sure an eventually-resolving primary doesn't crash the event loop
  // with "unhandled rejection" after we've already given up on it.
  primary.catch(() => undefined)

  const raced = await waitWithTimeout(primary, OBSERVATION_TIMEOUT_MS)
  // Two ways the primary is "bad": (a) the outer race fired (TIMEOUT_SENTINEL),
  // (b) the inner performActions deadline fired and returned a synthetic
  // empty observe-result with action_result_name === 'OBSERVATION_TIMEOUT'.
  // Either way we enter the fallback path.
  if (raced !== TIMEOUT_SENTINEL && !observeResultTimedOut(raced)) {
    logInfo('observationWithTimeout:primary_ok', {
      observeTabId,
      elapsedMs: Date.now() - startedAt,
      observationCount: raced.observations.length,
    })
    return {result: raced, timedOut: false}
  }

  const timeoutSource =
      raced === TIMEOUT_SENTINEL ? 'outer_race' : 'inner_deadline'
  logWarn('observationWithTimeout:primary_timeout', {
    observeTabId,
    elapsedMs: Date.now() - startedAt,
    timeoutSource,
  })

  // Timeout fired. Cancel the hung native action so the next tool call in
  // the enqueueForContext chain can actually run; race the cancel against
  // its own deadline in case the cancel itself has to queue. (If the
  // inner-deadline path already fired a cancel, a second one is harmless —
  // CancelOngoingActions is idempotent on the native side.)
  if (typeof host.cancelCurrentActions === 'function') {
    logInfo('observationWithTimeout:cancel_start', {observeTabId})
    const cancelStart = Date.now()
    const cancelPromise =
        host.cancelCurrentActions().catch(err => {
          logWarn('observationWithTimeout:cancel_failed', {
            observeTabId,
            error: String(err),
          })
          return undefined
        })
    const cancelResult = await waitWithTimeout(cancelPromise, CANCEL_TIMEOUT_MS)
    logInfo('observationWithTimeout:cancel_end', {
      observeTabId,
      elapsedMs: Date.now() - cancelStart,
      cancelTimedOut: cancelResult === TIMEOUT_SENTINEL,
    })
  } else {
    logWarn('observationWithTimeout:cancel_unavailable', {observeTabId})
  }

  // Fetch lightweight tab metadata in parallel with the retry so the model
  // gets something useful to look at. Both are best-effort; if metadata
  // doesn't come back we still return the retry observation.
  const hasMetadata = typeof host.getFocusedTabMetadata === 'function'
  logInfo('observationWithTimeout:metadata_start', {hasMetadata})
  const metadataPromise: Promise<ObservationTimeoutInfo|undefined> =
      hasMetadata
          ? host.getFocusedTabMetadata!().catch(err => {
              logWarn('observationWithTimeout:metadata_failed', {
                error: String(err),
              })
              return undefined
            })
          : Promise.resolve(undefined)

  const fallbackConfig: ObservationConfig = {
    ...observationConfig,
    skipAsyncObservation: true,
  }
  logInfo('observationWithTimeout:fallback_start', {observeTabId})
  const fallbackStart = Date.now()
  const fallbackResult =
      await executeObservation(host, fallbackConfig, observeTabId)
  logInfo('observationWithTimeout:fallback_end', {
    observeTabId,
    elapsedMs: Date.now() - fallbackStart,
    fallbackResultName: fallbackResult.action_result_name,
    fallbackObservationCount: fallbackResult.observations.length,
  })

  const metadata = await waitWithTimeout(metadataPromise, METADATA_TIMEOUT_MS)
  const timeoutInfo =
      metadata && metadata !== TIMEOUT_SENTINEL ? metadata : undefined
  logInfo('observationWithTimeout:end', {
    observeTabId,
    totalElapsedMs: Date.now() - startedAt,
    metadataResolved: timeoutInfo !== undefined,
    metadataTabId: timeoutInfo?.tab_id,
    metadataUrl: timeoutInfo?.url,
    metadataTitle: timeoutInfo?.title,
  })
  return {result: fallbackResult, timedOut: true, timeoutInfo}
}

function buildObservationTimeoutMessage(
    info: ObservationTimeoutInfo|undefined): string {
  const base =
      'The action completed, but collecting the post-action page observation timed out. ' +
      'This usually means the page is an error page, is still loading, or is otherwise ' +
      'stuck (no FirstContentfulPaint). '
  const where = info?.url || info?.title
      ? `Current page: ${info?.title ? `"${info.title}" ` : ''}` +
        `${info?.url ? `(${info.url})` : ''}. `
      : ''
  const followUp =
      'Call browser_screenshot or browser_read_page to inspect the page, ' +
      'or browser_navigate to go elsewhere.'
  return base + where + followUp
}

function filterObserveResultToTab(
    result: BrowserObserveResult,
    tabId: number): BrowserObserveResult {
  const matchingObservations =
      result.observations.filter(observation => observation.tab_id === tabId)
  if (matchingObservations.length === 0 ||
      matchingObservations.length === result.observations.length) {
    return result
  }

  return {
    ...result,
    observations: matchingObservations,
  }
}

function getFallbackWindowId(windows: WindowInfo[]): number|undefined {
  const activeWindow =
      windows.find(window => window.is_active && isPositiveInteger(window.id))
  if (activeWindow) {
    return activeWindow.id
  }

  const focusedWindow = windows.find(
      window =>
          isPositiveInteger(window.id) &&
          isPositiveInteger(window.active_tab_id))
  if (focusedWindow) {
    return focusedWindow.id
  }

  const firstWindow =
      windows.find(window => isPositiveInteger(window.id))
  return firstWindow?.id
}

function getFallbackTabId(windows: WindowInfo[]): number|undefined {
  const activeWindow = windows.find(
      window =>
          window.is_active && isPositiveInteger(window.active_tab_id))
  if (activeWindow) {
    return activeWindow.active_tab_id
  }

  const focusedWindow =
      windows.find(window => isPositiveInteger(window.active_tab_id))
  if (focusedWindow) {
    return focusedWindow.active_tab_id
  }

  for (const window of windows) {
    const firstTabId =
        window.tab_ids.find(tabId => isPositiveInteger(tabId))
    if (firstTabId !== undefined) {
      return firstTabId
    }
  }

  return undefined
}

function actionHasTabId(action: BrowserAction): action is BrowserActionWithTabId {
  return action.type !== 'wait' && action.type !== 'create_tab'
}

function getCreatedTabId(result: BrowserActResult): number|undefined {
  const observedTab = result.tabs.find(tab => isPositiveInteger(tab.id))
  if (observedTab) {
    return observedTab.id
  }

  return getFallbackTabId(result.windows)
}

async function executeCreateVisibleTab(
    host: HostedToolHost,
    windowId: number): Promise<{
      result: BrowserActResult;
      windowId: number;
    }> {
  let usedWindowId = windowId
  let result = await executeBrowserActions(host, [
    {
      type: 'create_tab',
      window_id: usedWindowId,
      foreground: true,
    },
  ])

  if (!result.success && result.action_result_name === 'WINDOW_WENT_AWAY') {
    const fallbackWindowId = getFallbackWindowId(result.windows)
    if (isPositiveInteger(fallbackWindowId) &&
        fallbackWindowId !== usedWindowId) {
      usedWindowId = fallbackWindowId
      result = await executeBrowserActions(host, [
        {
          type: 'create_tab',
          window_id: usedWindowId,
          foreground: true,
        },
      ])
    }
  }

  return {
    result,
    windowId: usedWindowId,
  }
}

async function resolveActionTabId(
    host: HostedToolHost,
    requestedTabId: unknown): Promise<number|undefined> {
  if (isPositiveInteger(requestedTabId)) {
    return requestedTabId
  }

  const status = await observeBrowserStatus(host)
  return getFallbackTabId(status.windows)
}

function normalizeSkillSource(value: unknown): string {
  return typeof value === 'number' ? (SKILL_SOURCE_NAMES[value] ?? `source_${value}`) :
                                     'unknown'
}

function normalizeSkillPreview(preview: Record<string, unknown>) {
  return {
    id: String(preview['id'] ?? ''),
    name: String(preview['name'] ?? ''),
    icon:
        typeof preview['icon'] === 'string' && preview['icon'] ? preview['icon'] :
                                                                 undefined,
    source: normalizeSkillSource(preview['source']),
    description:
        typeof preview['description'] === 'string' &&
            preview['description'] ?
        preview['description'] :
        undefined,
  }
}

function normalizeSkillRecord(skill: unknown) {
  if (!skill || typeof skill !== 'object' || Array.isArray(skill)) {
    return undefined
  }

  const typedSkill = skill as Record<string, unknown>
  const preview =
      typedSkill['preview'] && typeof typedSkill['preview'] === 'object' &&
          !Array.isArray(typedSkill['preview']) ?
      normalizeSkillPreview(typedSkill['preview'] as Record<string, unknown>) :
      undefined
  if (!preview) {
    return undefined
  }

  return {
    preview,
    prompt: String(typedSkill['prompt'] ?? ''),
    sourceSkillId:
        typeof typedSkill['sourceSkillId'] === 'string' &&
            typedSkill['sourceSkillId'] ?
        typedSkill['sourceSkillId'] :
        undefined,
  }
}

async function executeSkillTool(
    host: HostedToolHost,
    toolName: string,
    args: Record<string, unknown>,
): Promise<unknown> {
  switch (toolName) {
    case 'browser_skill_previews':
      return await host.listSkillPreviews()

    case 'browser_get_skill': {
      const id =
          typeof args['id'] === 'string' ? args['id'].trim() : ''
      if (!id) {
        throw new Error('INVALID_ARGS: Missing required skill id')
      }
      const skill = normalizeSkillRecord(await host.getSkill(id))
      if (!skill) {
        throw new Error(`NOT_FOUND: Skill '${id}' was not found`)
      }
      return skill
    }

    case 'browser_create_skill': {
      const prompt =
          typeof args['prompt'] === 'string' ? args['prompt'].trim() : ''
      if (!prompt) {
        throw new Error('INVALID_ARGS: Missing required skill prompt')
      }
      const result = await host.createSkill({
        prompt,
        id: typeof args['id'] === 'string' && args['id'].trim() ? args['id'].trim() :
                                                                   undefined,
        name:
            typeof args['name'] === 'string' && args['name'].trim() ? args['name'].trim() :
                                                                       undefined,
        icon:
            typeof args['icon'] === 'string' && args['icon'].trim() ? args['icon'].trim() :
                                                                       undefined,
        description:
            typeof args['description'] === 'string' &&
                args['description'].trim() ?
            args['description'].trim() :
            undefined,
      } satisfies HostedCreateSkillRequest)
      return {
        ok: true,
        modalOpened: result.modalOpened === true,
      }
    }

    case 'browser_manage_skills':
      return await host.showManageSkillsUi()

    default:
      throw new Error(`NOT_IMPLEMENTED: Unsupported skill tool '${toolName}'`)
  }
}

// =========================================================================
//  SimpleToolResult builders
// =========================================================================

function buildSimpleTabList(windows: WindowInfo[]): SimpleTabInfo[] {
  const tabs: SimpleTabInfo[] = []
  for (const win of windows) {
    for (const tabId of win.tab_ids) {
      if (!isPositiveInteger(tabId)) continue
      tabs.push({
        tab_id: tabId,
        active: tabId === win.active_tab_id && win.is_active,
      })
    }
  }
  return tabs
}

async function performObservation(
    host: HostedToolHost,
    registry: ElementRegistry,
    tabId?: number,
    hasScreenshot: boolean = false): Promise<{
      observation: SimpleObservation | undefined;
      tabs: SimpleTabInfo[];
      timedOut: boolean;
      timeoutInfo?: ObservationTimeoutInfo;
    }> {
  // Default for action-tool observations: only actionable elements. Text
  // nodes are dropped — Chromium's actor pipeline rejects non-WebElement
  // targets with ELEMENT_OFFSCREEN, so exposing them just lets the model
  // pick a target that can never click. Use browser_read_page,
  // browser_read_element, or browser_find_text to read body text.
  const basePreset = OBSERVATION_PRESETS.ACTIONABLE_ONLY
  const observationConfig: ObservationConfig = {
    ...basePreset,
    screenshot: { ...basePreset.screenshot, include: hasScreenshot },
  }
  let resolvedTabId = isPositiveInteger(tabId) ? tabId : undefined
  if (!isPositiveInteger(resolvedTabId)) {
    resolvedTabId = await resolveActionTabId(host, undefined)
  }
  const firstAttempt =
      await executeObservationWithTimeout(host, observationConfig, resolvedTabId)
  let result = firstAttempt.result
  const timedOut = firstAttempt.timedOut
  const timeoutInfo = firstAttempt.timeoutInfo

  if (!isPositiveInteger(resolvedTabId)) {
    resolvedTabId = getFallbackTabId(result.windows)
  }

  // Don't retry on an empty observation if the first attempt already fell
  // back on timeout — the skipAsyncObservation retry intentionally produces
  // no observation (just tab_id), so a follow-up full-observation would
  // just hang again.
  if (!timedOut && !isPositiveInteger(tabId) &&
      result.observations.length === 0) {
    if (isPositiveInteger(resolvedTabId)) {
      const retried =
          await executeObservation(host, observationConfig, resolvedTabId)
      if (retried.observations.length > 0) {
        result = retried
      }
    }
  }

  if (isPositiveInteger(resolvedTabId)) {
    result = filterObserveResultToTab(result, resolvedTabId)
  }

  const tabs = buildSimpleTabList(result.windows)
  let observation: SimpleObservation | undefined
  if (result.observations.length > 0) {
    observation = toSimpleObservation(
        result.observations[0]!, registry, observationConfig)
  }

  return { observation, tabs, timedOut, timeoutInfo }
}

function diffCreatedTabs(
    before: SimpleTabInfo[],
    after: SimpleTabInfo[]): SimpleTabInfo[] {
  const beforeIds = new Set(before.map(t => t.tab_id))
  return after.filter(t => !beforeIds.has(t.tab_id))
}

function buildResult(params: {
  ok: boolean
  resultName: string
  message?: string
  tabs?: SimpleTabInfo[]
  includeTabs?: boolean
  createdTabs?: SimpleTabInfo[]
  observation?: SimpleObservation
}): SimpleToolResult {
  const out: SimpleToolResult = { ok: params.ok, result: params.resultName }
  if (params.message) {
    out.message = params.message
  }
  if (params.observation) {
    out.observation = params.observation
  }
  if (params.includeTabs !== false && params.tabs && params.tabs.length > 0) {
    out.tabs = params.tabs
  }
  if (params.createdTabs && params.createdTabs.length > 0) {
    out.created_tabs = params.createdTabs
  }
  return out
}

async function executeCreateTabTool(
    host: HostedToolHost,
    registry: ElementRegistry,
    args: Record<string, unknown>): Promise<SimpleToolResult> {
  await host.resetTask('create_tab_pre_reset')

  if (!isNonEmptyString(args['url'])) {
    throw new Error(
        'INVALID_ARGS: browser_create_tab requires a non-empty url')
  }
  const url = args['url'].trim()
  const reuseVisibleTab = args['reuse_visible_tab'] === true

  const preStatus = await observeBrowserStatus(host)
  const preActionTabs = buildSimpleTabList(preStatus.windows)

  const {
    tabId: activeTabId,
    windowId: activeWindowId,
  } = {
    tabId: getFallbackTabId(preStatus.windows),
    windowId: getFallbackWindowId(preStatus.windows),
  }

  if (reuseVisibleTab) {
    if (!isPositiveInteger(activeTabId)) {
      throw new Error(
          'INVALID_ARGS: browser_create_tab could not resolve the active visible tab')
    }
    const navigateResult = await executeBrowserActions(host, [
      { type: 'navigate', tab_id: activeTabId, url },
    ])
    const status = await observeBrowserStatus(host)
    const tabs = buildSimpleTabList(status.windows)
    const createdTabs = diffCreatedTabs(preActionTabs, tabs)
    let observation: SimpleObservation | undefined
    let observationTimedOut = false
    let observationTimeoutInfo: ObservationTimeoutInfo | undefined
    if (navigateResult.success) {
      const obs =
          await performObservation(host, registry, activeTabId)
      observation = obs.observation
      observationTimedOut = obs.timedOut
      observationTimeoutInfo = obs.timeoutInfo
      if (obs.tabs.length > 0) tabs.splice(0, tabs.length, ...obs.tabs)
    }
    return buildResult({
      ok: navigateResult.success,
      resultName: navigateResult.action_result_name,
      tabs,
      includeTabs: !navigateResult.success,
      createdTabs,
      observation,
      message: observationTimedOut
          ? buildObservationTimeoutMessage(observationTimeoutInfo)
          : undefined,
    })
  }

  if (!isPositiveInteger(activeWindowId)) {
    throw new Error(
        'INVALID_ARGS: browser_create_tab requires a resolvable active browser window')
  }

  const { result: createResult } =
      await executeCreateVisibleTab(host, activeWindowId)
  let createdTabId = getCreatedTabId(createResult)
  if (!isPositiveInteger(createdTabId) && createResult.success) {
    createdTabId = await resolveActionTabId(host, undefined)
  }

  if (!createResult.success || !isPositiveInteger(createdTabId)) {
    const status = await observeBrowserStatus(host)
    const tabs = buildSimpleTabList(status.windows)
    return buildResult({
      ok: false,
      resultName:
          createResult.success ? 'TAB_ID_UNAVAILABLE' : createResult.action_result_name,
      tabs,
      includeTabs: true,
      createdTabs: diffCreatedTabs(preActionTabs, tabs),
    })
  }

  const navigateResult = await executeBrowserActions(host, [
    { type: 'navigate', tab_id: createdTabId, url },
  ])
  const status = await observeBrowserStatus(host)
  const tabs = buildSimpleTabList(status.windows)
  const createdTabs = diffCreatedTabs(preActionTabs, tabs)
  let observation: SimpleObservation | undefined
  let observationTimedOut = false
  let observationTimeoutInfo: ObservationTimeoutInfo | undefined
  if (navigateResult.success) {
    const obs =
        await performObservation(host, registry, createdTabId)
    observation = obs.observation
    observationTimedOut = obs.timedOut
    observationTimeoutInfo = obs.timeoutInfo
    if (obs.tabs.length > 0) tabs.splice(0, tabs.length, ...obs.tabs)
  }
  return buildResult({
    ok: navigateResult.success,
    resultName: navigateResult.action_result_name,
    tabs,
    includeTabs: !navigateResult.success,
    createdTabs,
    observation,
    message: observationTimedOut
        ? buildObservationTimeoutMessage(observationTimeoutInfo)
        : undefined,
  })
}

async function executeScreenshotTool(
    host: HostedToolHost,
    registry: ElementRegistry,
    args: Record<string, unknown>): Promise<SimpleToolResult> {
  const requestedTabId =
      isPositiveInteger(args['tab_id']) ? args['tab_id'] : undefined
  const { observation, tabs, timedOut, timeoutInfo } =
      await performObservation(host, registry, requestedTabId, true)
  return buildResult({
    ok: true,
    resultName: timedOut ? 'OBSERVATION_TIMEOUT' : 'OK',
    tabs,
    includeTabs: !observation,
    observation,
    message: timedOut
        ? buildObservationTimeoutMessage(timeoutInfo)
        : undefined,
  })
}

// =========================================================================
//  Read-only tools (no actor pipeline, no ELEMENT_OFFSCREEN possible)
// =========================================================================

const READ_PAGE_DEFAULT_MAX_CHARS = 4000
const READ_ELEMENT_DEFAULT_MAX_CHARS = 2000
const FIND_TEXT_DEFAULT_MAX_RESULTS = 5
const FIND_TEXT_HARD_CAP = 20

/**
 * Issue a fresh observation with no element filtering and return the full
 * PageObservation for the chosen tab. Used by the read tools to access text
 * nodes that are intentionally hidden from action-tool observations.
 *
 * Falls back to undefined when no observation is produced (tab gone, etc.).
 */
async function fetchFullObservation(
    host: HostedToolHost,
    tabId: number | undefined): Promise<{
      observation: PageObservation | undefined;
      timedOut: boolean;
      timeoutInfo?: ObservationTimeoutInfo;
    }> {
  const observationConfig: ObservationConfig = OBSERVATION_PRESETS.FULL_FOR_READ
  let resolvedTabId = isPositiveInteger(tabId) ? tabId : undefined
  if (!isPositiveInteger(resolvedTabId)) {
    resolvedTabId = await resolveActionTabId(host, undefined)
  }
  const attempt =
      await executeObservationWithTimeout(host, observationConfig, resolvedTabId)
  const result = attempt.result
  if (result.observations.length === 0) {
    return {
      observation: undefined,
      timedOut: attempt.timedOut,
      timeoutInfo: attempt.timeoutInfo,
    }
  }
  let match: PageObservation | undefined
  if (isPositiveInteger(resolvedTabId)) {
    match = result.observations.find(o => o.tab_id === resolvedTabId)
  }
  return {
    observation: match ?? result.observations[0],
    timedOut: attempt.timedOut,
    timeoutInfo: attempt.timeoutInfo,
  }
}

async function executeReadPageTool(
    host: HostedToolHost,
    args: Record<string, unknown>): Promise<SimpleToolResult> {
  const requestedTabId =
      isPositiveInteger(args['tab_id']) ? args['tab_id'] : undefined
  const scope = args['scope'] === 'full_page' ? 'full_page' : 'viewport'
  const maxChars = isPositiveInteger(args['max_chars'])
      ? args['max_chars']
      : READ_PAGE_DEFAULT_MAX_CHARS

  const fetched = await fetchFullObservation(host, requestedTabId)
  const observation = fetched.observation
  if (!observation) {
    const baseMsg = fetched.timedOut
        ? buildObservationTimeoutMessage(fetched.timeoutInfo)
        : 'browser_read_page: failed to fetch a page observation.'
    return buildResult({
      ok: false,
      resultName: fetched.timedOut ? 'OBSERVATION_TIMEOUT' : 'NO_OBSERVATION',
      message: baseMsg,
    })
  }

  const { text, truncated } = joinPageVisibleText(observation.elements, {
    maxChars,
    viewportOnly: scope === 'viewport',
  })

  const out: SimpleToolResult = {
    ok: true,
    result: 'OK',
    page_text: text,
  }
  if (truncated) out.truncated = true
  return out
}

async function executeReadElementTool(
    host: HostedToolHost,
    registry: ElementRegistry,
    args: Record<string, unknown>): Promise<SimpleToolResult> {
  const elementId = args['element_id']
  if (!isPositiveInteger(elementId)) {
    return buildResult({
      ok: false,
      resultName: 'INVALID_ARGS',
      message: 'browser_read_element: missing required element_id.',
    })
  }
  let entry
  try {
    entry = registry.resolve(elementId)
  } catch (err) {
    const msg = err instanceof Error ? err.message : String(err)
    return buildResult({
      ok: false,
      resultName: 'INVALID_ELEMENT_ID',
      message: msg,
    })
  }
  const maxChars = isPositiveInteger(args['max_chars'])
      ? args['max_chars']
      : READ_ELEMENT_DEFAULT_MAX_CHARS

  const fetched = await fetchFullObservation(host, entry.tab_id)
  const observation = fetched.observation
  if (!observation) {
    const baseMsg = fetched.timedOut
        ? buildObservationTimeoutMessage(fetched.timeoutInfo)
        : 'browser_read_element: failed to fetch a page observation.'
    return buildResult({
      ok: false,
      resultName: fetched.timedOut ? 'OBSERVATION_TIMEOUT' : 'NO_OBSERVATION',
      message: baseMsg,
    })
  }

  // Element matching is by node_id; document_id mismatch means navigation
  // happened since the registered observation, in which case the element
  // is no longer on the page. Surface a recognizable error so the model
  // re-observes instead of guessing.
  if (entry.document_id &&
      observation.frame &&
      observation.frame.document_id !== entry.document_id) {
    return buildResult({
      ok: false,
      resultName: 'STALE_DOCUMENT',
      message:
          'browser_read_element: the page navigated since the element_id was issued. ' +
          'Call any browser_* tool to get a fresh observation, then retry.',
    })
  }

  const idx = observation.elements.findIndex(e => e.node_id === entry.node_id)
  if (idx < 0) {
    return buildResult({
      ok: false,
      resultName: 'ELEMENT_NOT_FOUND',
      message:
          'browser_read_element: element is no longer present in the live DOM. ' +
          'Re-observe and try again.',
    })
  }

  const { text, truncated } = collectElementText(observation.elements, idx, maxChars)
  const out: SimpleToolResult = {
    ok: true,
    result: 'OK',
    element_text: text,
  }
  if (truncated) out.truncated = true
  return out
}

/** Lowercased substring match — case- and whitespace-tolerant. */
function makeTextMatcher(query: string): (s: string | undefined) => boolean {
  const needle = query.trim().toLowerCase()
  if (!needle) return () => false
  return (s) => {
    if (!s) return false
    return s.toLowerCase().includes(needle)
  }
}

function elementVisibleText(e: PageElement): string | undefined {
  return e.text || e.label || e.title || undefined
}

async function executeFindTextTool(
    host: HostedToolHost,
    registry: ElementRegistry,
    args: Record<string, unknown>): Promise<SimpleToolResult> {
  const query = typeof args['query'] === 'string' ? args['query'].trim() : ''
  if (!query) {
    return buildResult({
      ok: false,
      resultName: 'INVALID_ARGS',
      message: 'browser_find_text: missing required query.',
    })
  }
  const requestedTabId =
      isPositiveInteger(args['tab_id']) ? args['tab_id'] : undefined
  const requested = isPositiveInteger(args['max_results'])
      ? args['max_results']
      : FIND_TEXT_DEFAULT_MAX_RESULTS
  const maxResults = Math.min(requested, FIND_TEXT_HARD_CAP)

  const fetched = await fetchFullObservation(host, requestedTabId)
  const observation = fetched.observation
  if (!observation) {
    const baseMsg = fetched.timedOut
        ? buildObservationTimeoutMessage(fetched.timeoutInfo)
        : 'browser_find_text: failed to fetch a page observation.'
    return buildResult({
      ok: false,
      resultName: fetched.timedOut ? 'OBSERVATION_TIMEOUT' : 'NO_OBSERVATION',
      message: baseMsg,
    })
  }

  const matcher = makeTextMatcher(query)
  const elements = observation.elements
  // Precompute parent indices once. findEnclosingElementIndex walks this
  // array instead of rescanning the full element list on every call — turns
  // the inner loop from O(n) per match to O(ancestor-depth).
  const parentIndex = buildParentIndex(elements)
  const matches: FindTextMatch[] = []
  // Ancestor → registered element_id cache to avoid re-registering the same
  // ancestor for several text matches that share a parent (very common on
  // suggestion lists).
  const ancestorIdCache = new Map<number, number>()
  // tab_id and document_id come from the observation we just fetched, so
  // every newly-registered ancestor will have a valid (live) document.
  const tabId = observation.tab_id
  const documentId = observation.frame?.document_id ?? ''

  for (let i = 0; i < elements.length && matches.length < maxResults; i++) {
    const el = elements[i]!
    const visible = elementVisibleText(el)
    if (!matcher(visible)) continue

    const match: FindTextMatch = {
      text: (visible ?? '').slice(0, 200),
    }
    const bb = el.visible_bbox ?? el.bbox
    if (bb && bb.width > 0 && bb.height > 0) {
      match.box = [bb.x, bb.y, bb.width, bb.height]
    }

    // If the matched element is itself actionable, register it directly.
    // Otherwise walk to the nearest enclosing actionable ancestor — the model
    // must click the ancestor (the actor pipeline rejects text-node targets
    // with ELEMENT_OFFSCREEN).
    const ancestorIdx = isActionable(el)
        ? i
        : findEnclosingElementIndex(elements, parentIndex, i, isActionable)
    if (ancestorIdx >= 0) {
      let registeredId = ancestorIdCache.get(ancestorIdx)
      if (registeredId == null) {
        const ancestor = elements[ancestorIdx]!
        const [mapped] = registry.registerObservation(tabId, documentId, [ancestor])
        registeredId = mapped!.element_id
        ancestorIdCache.set(ancestorIdx, registeredId)
      }
      match.clickable_ancestor_id = registeredId
      const ancestor = elements[ancestorIdx]!
      match.ancestor_type = ancestor.type
      const ancestorText = elementVisibleText(ancestor)
      if (ancestorText) match.ancestor_text = ancestorText.slice(0, 120)
    }

    matches.push(match)
  }

  return {
    ok: true,
    result: matches.length > 0 ? 'OK' : 'NO_MATCH',
    matches,
  }
}

async function executeActionTool(
    host: HostedToolHost,
    registry: ElementRegistry,
    toolName: string,
    args: Record<string, unknown>): Promise<SimpleToolResult> {
  if (toolName === 'browser_create_tab') {
    return executeCreateTabTool(host, registry, args)
  }
  if (toolName === 'browser_screenshot') {
    return executeScreenshotTool(host, registry, args)
  }
  if (toolName === 'browser_read_page') {
    return executeReadPageTool(host, args)
  }
  if (toolName === 'browser_read_element') {
    return executeReadElementTool(host, registry, args)
  }
  if (toolName === 'browser_find_text') {
    return executeFindTextTool(host, registry, args)
  }

  const parsed = parseSimpleToolCall(toolName, args, registry)
  if (isSimpleToolCallError(parsed)) {
    return buildResult({
      ok: false,
      resultName: 'INVALID_ARGS',
      message: parsed.error,
    })
  }
  if (!parsed) {
    throw new Error(`NOT_IMPLEMENTED: Unsupported browser tool '${toolName}'`)
  }

  // For tab-level tools without a tab_id, resolve the active tab
  const needsTabFallback =
      actionHasTabId(parsed) && !isPositiveInteger(parsed.tab_id)
  if (needsTabFallback) {
    const fallbackTabId = await resolveActionTabId(host, undefined)
    if (isPositiveInteger(fallbackTabId)) {
      parsed.tab_id = fallbackTabId
    }
  }

  // For scroll without element_id, resolve viewport center of active tab
  if (parsed.type === 'scroll' && 'target' in parsed &&
      'x' in parsed.target && parsed.target.x === 0 && parsed.target.y === 0) {
    const scrollTabId = 'tab_id' in parsed && isPositiveInteger(parsed.tab_id)
        ? parsed.tab_id : undefined
    const resolvedTab = await resolveActionTabId(host, scrollTabId)
    if (isPositiveInteger(resolvedTab)) {
      (parsed as { tab_id: number }).tab_id = resolvedTab
    }
  }

  const phaseTabId = 'tab_id' in parsed && isPositiveInteger(parsed.tab_id)
      ? parsed.tab_id
      : undefined
  logInfo('actionTool:phase1_preStatus', {toolName, tabId: phaseTabId})
  const preStatus = await observeBrowserStatus(host)
  const preActionTabs = buildSimpleTabList(preStatus.windows)

  logInfo('actionTool:phase2_action', {
    toolName,
    parsedType: parsed.type,
    tabId: phaseTabId,
  })
  const result = await executeBrowserActions(host, [parsed])
  logInfo('actionTool:phase2_action_done', {
    toolName,
    success: result.success,
    resultName: result.action_result_name,
  })

  if (toolName === 'browser_type' &&
      !result.success &&
      result.action_result_name === 'TYPE_UNSUPPORTED_CHARACTERS') {
    const status = await observeBrowserStatus(host)
    const tabs = buildSimpleTabList(status.windows)
    return buildResult({
      ok: false,
      resultName: 'TYPE_UNSUPPORTED_CHARACTERS',
      tabs,
      includeTabs: true,
      createdTabs: diffCreatedTabs(preActionTabs, tabs),
    })
  }

  if (!result.success &&
      result.action_result_name === 'ARGUMENTS_INVALID') {
    const resolvedTarget = 'target' in parsed && parsed.target
        ? parsed.target : undefined
    const targetInfo = resolvedTarget && 'node_id' in resolvedTarget
        ? { node_id: resolvedTarget.node_id, document_id: resolvedTarget.document_id }
        : undefined
    const tabId = 'tab_id' in parsed && isPositiveInteger(parsed.tab_id)
        ? parsed.tab_id : undefined
    console.warn(
        '[browser-actor] ARGUMENTS_INVALID for', toolName,
        '| tab_id:', tabId,
        '| target:', JSON.stringify(targetInfo))

    let diagnosticMessage =
        'ARGUMENTS_INVALID: The browser rejected the action arguments.'
    if (tabId) {
      try {
        const diagObs = await performObservation(host, registry, tabId)
        if (diagObs.timedOut) {
          diagnosticMessage += ' ' +
              buildObservationTimeoutMessage(diagObs.timeoutInfo)
        } else if (diagObs.observation) {
          const docChanged = targetInfo?.document_id &&
              diagObs.observation.url !== ''
          diagnosticMessage +=
              ' The page may have changed since the last observation.' +
              ' Re-issue the action — every browser_* tool returns a fresh observation.'
          if (docChanged) {
            console.warn(
                '[browser-actor] Diagnostic re-observe suggests document changed.',
                'Original document_id:', targetInfo?.document_id)
          }
        }
      } catch {
        // Diagnostic re-observe failed — proceed with the original error
      }
    }

    const status = await observeBrowserStatus(host)
    const tabs = buildSimpleTabList(status.windows)
    return buildResult({
      ok: false,
      resultName: 'ARGUMENTS_INVALID',
      tabs,
      includeTabs: true,
      createdTabs: diffCreatedTabs(preActionTabs, tabs),
      message: diagnosticMessage,
    })
  }

  logInfo('actionTool:phase3_postStatus', {toolName})
  const status = await observeBrowserStatus(host)
  const tabs = buildSimpleTabList(status.windows)
  const createdTabs = diffCreatedTabs(preActionTabs, tabs)

  let observation: SimpleObservation | undefined
  let observationTimedOut = false
  let observationTimeoutInfo: ObservationTimeoutInfo | undefined
  if (result.success) {
    const targetTabId =
        parsed.type === 'wait' && typeof parsed.observe_tab_id === 'number'
            ? parsed.observe_tab_id
            : ('tab_id' in parsed && isPositiveInteger(parsed.tab_id)
                ? parsed.tab_id : undefined)
    logInfo('actionTool:phase4_observation', {toolName, targetTabId})
    const obs =
        await performObservation(host, registry, targetTabId)
    observation = obs.observation
    observationTimedOut = obs.timedOut
    observationTimeoutInfo = obs.timeoutInfo
    if (obs.tabs.length > 0) tabs.splice(0, tabs.length, ...obs.tabs)
    logInfo('actionTool:phase4_observation_done', {
      toolName,
      timedOut: observationTimedOut,
      hasObservation: observation !== undefined,
      elementCount: observation?.elements.length ?? 0,
      timeoutTabId: observationTimeoutInfo?.tab_id,
      timeoutUrl: observationTimeoutInfo?.url,
    })
  } else {
    logInfo('actionTool:phase4_observation_skipped', {
      toolName,
      reason: 'action_failed',
    })
  }

  return buildResult({
    ok: result.success,
    resultName: result.action_result_name,
    tabs,
    includeTabs: !result.success,
    createdTabs,
    observation,
    message: observationTimedOut
        ? buildObservationTimeoutMessage(observationTimeoutInfo)
        : undefined,
  })
}

/**
 * Execute a hosted tool call. Browser-actor tools return a SimpleToolResult;
 * skill tools return their own payload shape. Element-targeting tools resolve
 * their synthetic element_id through the provided registry.
 *
 * @param host     - The browser host interface (Glic actor).
 * @param request  - The tool name and arguments from the LLM.
 * @param registry - Element registry for synthetic element_id resolution.
 */
export async function executeHostedTool(
    host: HostedToolHost,
    request: HostedToolExecutionRequest,
    registry: ElementRegistry): Promise<unknown> {
  const toolName = request.name.trim()
  if (!toolName) {
    throw new Error('INVALID_ARGS: Missing required tool name')
  }

  const args = normalizeArgs(request.args)
  const startedAt = Date.now()
  // Compact arg snapshot — keep tab_id / element_id / url / query for cheap
  // correlation in logs, drop anything potentially PII-heavy (text, prompts).
  const argSummary: Record<string, unknown> = {}
  if (args['tab_id'] !== undefined) argSummary['tab_id'] = args['tab_id']
  if (args['element_id'] !== undefined) argSummary['element_id'] = args['element_id']
  if (typeof args['url'] === 'string') argSummary['url'] = args['url']
  if (typeof args['query'] === 'string') argSummary['query'] = args['query']
  if (typeof args['direction'] === 'string') argSummary['direction'] = args['direction']
  logInfo('executeHostedTool:begin', {toolName, args: argSummary})

  try {
    const isBrowserTool = isBrowserActorTool(toolName)
    const out = isBrowserTool
        ? await executeActionTool(host, registry, toolName, args)
        : await executeSkillTool(host, toolName, args)

    const summary: Record<string, unknown> = {
      toolName,
      elapsedMs: Date.now() - startedAt,
    }
    if (isBrowserTool && out && typeof out === 'object') {
      const r = out as SimpleToolResult
      summary['ok'] = r.ok
      summary['result'] = r.result
      summary['hasMessage'] = r.message !== undefined
      summary['hasObservation'] = r.observation !== undefined
    }
    logInfo('executeHostedTool:end', summary)
    return out
  } catch (error: unknown) {
    logError('executeHostedTool:throw', {
      toolName,
      elapsedMs: Date.now() - startedAt,
      error: error instanceof Error ? error.message : String(error),
    })
    throw error
  }
}

export function getHostedToolNames(definitions: { name: string }[]): string[] {
  return definitions.map(tool => tool.name)
}
