// Copyright 2024 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Manages the hosted actor task lifecycle for Nexus browser-owned tools.
// Extracted from nexus_boot.ts to keep the boot file focused on
// initialization, guest handshake, and bridge plumbing.

import {
  ElementRegistry,
  type HostedToolHost,
} from './browser-actor-tools/index.js';
import {NEXUS_DEBUG, nexusTrace} from './nexusTrace.js';

// =========================================================================
//  Constants
// =========================================================================

const HOSTED_ACTOR_DEFAULT_CONTEXT_KEY = '__default__';
const CREATE_TASK_ERROR_EXISTING_ACTIVE_TASK = 2;
const ACTOR_TASK_STOP_REASON_STOPPED_BY_USER = 1;

// =========================================================================
//  Types
// =========================================================================

export type HostedToolExecutionContext = {
  sessionId?: string;
  providerId?: string;
  modelId?: string;
};

export type HostedActorTaskState = {
  taskId: number | null;
  createTaskPromise: Promise<number> | null;
  apiHost: any | null;
  registry: ElementRegistry;
  // In-flight performActions promises for this contextKey. Stop/reset paths
  // must await these before sending glicBrowserStopActorTask so the browser
  // never sees a stop while the Mojo response callback for the current
  // PerformActions is still pending-and-referenced by a weak_ptr continuation
  // in GlicActorTaskManager. (See glic.mojom.cc ProxyToResponder DCHECK.)
  pendingActions: Set<Promise<unknown>>;
};

// =========================================================================
//  Module state
// =========================================================================

const hostedActorTaskStates = new Map<string, HostedActorTaskState>();
let activeHostedActorContextKey: string | null = null;

// =========================================================================
//  Utilities
// =========================================================================

function isPositiveInteger(value: unknown): value is number {
  return typeof value === 'number' && Number.isInteger(value) && value > 0;
}

export async function callApiHostRequest(
    apiHost: any, type: string, payload?: unknown): Promise<any> {
  const extras = {
    addTransfer(_transfer: unknown) {},
    transfers: [],
  };
  const response = await apiHost.handleRawRequest(type, payload, extras);
  return response?.payload;
}

function readErrorReason(error: unknown): {reasonType?: string, reason?: number} {
  if (!error || typeof error !== 'object') {
    return {};
  }
  const typedError = error as Record<string, unknown>;
  return {
    reasonType:
        typeof typedError['reasonType'] === 'string' ? typedError['reasonType'] :
                                                       undefined,
    reason:
        typeof typedError['reason'] === 'number' ? typedError['reason'] : undefined,
  };
}

function normalizeSkillPreviews(previews: unknown): unknown[] {
  if (!Array.isArray(previews)) {
    return [];
  }
  return previews.filter(preview => !!preview && typeof preview === 'object');
}

// =========================================================================
//  Task state helpers
// =========================================================================

function createHostedActorTaskState(): HostedActorTaskState {
  return {
    taskId: null,
    createTaskPromise: null,
    apiHost: null,
    registry: new ElementRegistry(),
    pendingActions: new Set(),
  };
}

async function drainPendingActions(state: HostedActorTaskState): Promise<void> {
  if (state.pendingActions.size === 0) {
    return;
  }
  const snapshot = Array.from(state.pendingActions);
  await Promise.allSettled(snapshot);
}

export function getHostedActorTaskState(contextKey: string): HostedActorTaskState {
  let state = hostedActorTaskStates.get(contextKey);
  if (!state) {
    state = createHostedActorTaskState();
    hostedActorTaskStates.set(contextKey, state);
  }
  return state;
}

function clearHostedActorTaskState(state: HostedActorTaskState): void {
  state.taskId = null;
  state.createTaskPromise = null;
  state.apiHost = null;
  state.registry.clear();
  // Pending-action set is not cleared here: the tracked promises must remain
  // observable so concurrent drain() callers can still await them. Individual
  // entries remove themselves on settle via the performActions wrapper.
}

// =========================================================================
//  Execution context helpers
// =========================================================================

export function normalizeHostedToolExecutionContext(value: unknown):
    HostedToolExecutionContext {
  if (!value || typeof value !== 'object' || Array.isArray(value)) {
    return {};
  }
  const v = value as Record<string, unknown>;
  return {
    sessionId:
        typeof v['sessionId'] === 'string' && v['sessionId'].trim() ?
        v['sessionId'].trim() : undefined,
    providerId:
        typeof v['providerId'] === 'string' && v['providerId'].trim() ?
        v['providerId'].trim() : undefined,
    modelId:
        typeof v['modelId'] === 'string' && v['modelId'].trim() ?
        v['modelId'].trim() : undefined,
  };
}

export function getHostedActorContextKey(
    executionContext: HostedToolExecutionContext): string {
  return executionContext.sessionId ?? HOSTED_ACTOR_DEFAULT_CONTEXT_KEY;
}

// =========================================================================
//  Task lifecycle management
// =========================================================================

function resetHostedActorTaskTracking(contextKey?: string): void {
  for (const [trackedContextKey, trackedState] of hostedActorTaskStates) {
    clearHostedActorTaskState(trackedState);
    if (trackedContextKey !== contextKey) {
      hostedActorTaskStates.delete(trackedContextKey);
    }
  }
  activeHostedActorContextKey = null;
}

function dropOtherHostedActorTaskStates(activeContextKey: string): void {
  for (const [trackedContextKey, trackedState] of hostedActorTaskStates) {
    if (trackedContextKey === activeContextKey) {
      continue;
    }
    clearHostedActorTaskState(trackedState);
    hostedActorTaskStates.delete(trackedContextKey);
  }
}

function readApiHostActorTaskIds(apiHost: any):
    {activeTaskId?: number, lastTaskId?: number} {
  if (!apiHost || typeof apiHost !== 'object' ||
      typeof apiHost.nexusActorGetStatus !== 'function') {
    return {};
  }
  try {
    const status = apiHost.nexusActorGetStatus();
    if (!status || typeof status !== 'object') {
      return {};
    }
    const s = status as Record<string, unknown>;
    return {
      activeTaskId:
          isPositiveInteger(s['activeTaskId']) ? s['activeTaskId'] : undefined,
      lastTaskId:
          isPositiveInteger(s['lastTaskId']) ? s['lastTaskId'] : undefined,
    };
  } catch (error: unknown) {
    if (NEXUS_DEBUG) {
      nexusTrace('nexusActorGetStatus failed', {
        message: error instanceof Error ? error.message : String(error),
      });
    }
    return {};
  }
}

async function stopUnknownHostedActorTask(apiHost: any, contextKey: string):
    Promise<boolean> {
  const {activeTaskId, lastTaskId} = readApiHostActorTaskIds(apiHost);
  const taskId = activeTaskId ?? lastTaskId;
  if (NEXUS_DEBUG) {
    nexusTrace('attempting stale hosted actor task recovery', {
      contextKey, activeTaskId, lastTaskId,
    });
  }

  if (isPositiveInteger(taskId)) {
    try {
      await callApiHostRequest(apiHost, 'glicBrowserStopActorTask', {
        taskId,
        stopReason: ACTOR_TASK_STOP_REASON_STOPPED_BY_USER,
      });
      return true;
    } catch (error: unknown) {
      if (NEXUS_DEBUG) {
        nexusTrace('stale hosted actor stop by taskId failed', {
          contextKey, taskId,
          message: error instanceof Error ? error.message : String(error),
        });
      }
    }
  }

  try {
    if (typeof apiHost.stopActorTask === 'function') {
      await Promise.resolve(apiHost.stopActorTask(
          undefined, ACTOR_TASK_STOP_REASON_STOPPED_BY_USER));
      return true;
    }
  } catch (error: unknown) {
    if (NEXUS_DEBUG) {
      nexusTrace('stale hosted actor direct stop failed', {
        contextKey,
        message: error instanceof Error ? error.message : String(error),
      });
    }
  }

  try {
    await callApiHostRequest(apiHost, 'glicBrowserStopActorTask', {
      stopReason: ACTOR_TASK_STOP_REASON_STOPPED_BY_USER,
    });
    return true;
  } catch (error: unknown) {
    if (NEXUS_DEBUG) {
      nexusTrace('stale hosted actor raw stop failed', {
        contextKey,
        message: error instanceof Error ? error.message : String(error),
      });
    }
  }

  return false;
}

function buildCreateTaskPayload(title?: string): {taskOptions?: {title: string}} {
  return title && title.trim() ? {taskOptions: {title: title.trim()}} :
                                 {taskOptions: undefined};
}

async function requestHostedActorTaskId(
    apiHost: any, title?: string): Promise<number> {
  const response = await callApiHostRequest(
      apiHost, 'glicBrowserCreateTask', buildCreateTaskPayload(title));
  const taskId = response?.taskId;
  if (!isPositiveInteger(taskId)) {
    throw new Error('createTask: failed');
  }
  return taskId;
}

async function resetHostedActorTaskState(
    contextKey: string, reason: string,
    options?: {removeState?: boolean}): Promise<void> {
  const state = hostedActorTaskStates.get(contextKey);
  if (!state) {
    return;
  }
  const removeState = options?.removeState === true;

  // Let any in-flight performActions resolve naturally before we tell the
  // browser to stop the task. If we stop first, the browser-side Act-callback
  // path can race with GlicActorTaskManager's weak_ptr-bound observation
  // continuation and drop the Mojo response callback while the pipe is still
  // bound — triggering the ProxyToResponder DCHECK in glic.mojom.cc.
  await drainPendingActions(state);

  if (state.taskId === null || !state.apiHost) {
    clearHostedActorTaskState(state);
    if (removeState) {
      hostedActorTaskStates.delete(contextKey);
    }
    if (activeHostedActorContextKey === contextKey) {
      activeHostedActorContextKey = null;
    }
    return;
  }

  const taskId = state.taskId;
  const apiHost = state.apiHost;
  if (NEXUS_DEBUG) {
    nexusTrace('resetting hosted actor task', {reason, taskId, contextKey});
  }

  try {
    await callApiHostRequest(apiHost, 'glicBrowserStopActorTask', {
      taskId,
      stopReason: ACTOR_TASK_STOP_REASON_STOPPED_BY_USER,
    });
  } catch (error: unknown) {
    if (NEXUS_DEBUG) {
      nexusTrace('resetHostedActorTask failed', {
        reason, taskId, contextKey,
        message: error instanceof Error ? error.message : String(error),
      });
    }
  }

  clearHostedActorTaskState(state);
  if (removeState) {
    hostedActorTaskStates.delete(contextKey);
  }
  if (activeHostedActorContextKey === contextKey) {
    activeHostedActorContextKey = null;
  }
}

// Abandon the current taskId. Used when the owning Mojo pipe is going away
// (apiHost_changed, pagehide, beforeunload) — the browser-side WebClientHandler
// receiver will disconnect, so we clear local state synchronously and let the
// stop request fly best-effort. Callers may await the returned promise, but
// unload-path callers won't (the event loop is about to stop anyway).
function stopHostedActorTaskStateIfNeeded(
    contextKey: string, reason: string,
    options?: {removeState?: boolean}): Promise<void> {
  const state = hostedActorTaskStates.get(contextKey);
  if (!state || state.taskId === null || !state.apiHost) {
    return Promise.resolve();
  }
  const removeState = options?.removeState === true;

  const taskId = state.taskId;
  const apiHost = state.apiHost;
  const pending = Array.from(state.pendingActions);
  if (NEXUS_DEBUG) {
    nexusTrace('stopping hosted actor task', {
      reason, taskId, contextKey, pendingActions: pending.length,
    });
  }

  // Clear local state synchronously so the NEW apiHost (if any) can start a
  // fresh task without reusing the abandoned id.
  clearHostedActorTaskState(state);
  if (removeState) {
    hostedActorTaskStates.delete(contextKey);
  }
  if (activeHostedActorContextKey === contextKey) {
    activeHostedActorContextKey = null;
  }

  return (async () => {
    // Drain any captured in-flight performActions BEFORE sending the stop.
    // Even for a dying pipe this matters: if the pipe is still alive and the
    // stop lands during a pending PerformActions observation-build, the upstream
    // GlicActorTaskManager can drop the Mojo response callback while the
    // WebClientHandler receiver is still bound — the ProxyToResponder DCHECK.
    if (pending.length > 0) {
      await Promise.allSettled(pending);
    }

    try {
      await callApiHostRequest(apiHost, 'glicBrowserStopActorTask', {
        taskId,
        stopReason: ACTOR_TASK_STOP_REASON_STOPPED_BY_USER,
      });
    } catch (error: unknown) {
      if (NEXUS_DEBUG) {
        nexusTrace('stopHostedActorTaskIfNeeded failed', {
          reason, taskId, contextKey,
          message: error instanceof Error ? error.message : String(error),
        });
      }
    }
  })();
}

async function resetOtherHostedActorTasks(
    activeContextKey: string, reason: string): Promise<void> {
  const resets: Promise<void>[] = [];
  for (const [contextKey, state] of hostedActorTaskStates) {
    if (contextKey === activeContextKey) {
      continue;
    }
    if (state.taskId === null && !state.apiHost && !state.createTaskPromise) {
      hostedActorTaskStates.delete(contextKey);
      continue;
    }
    resets.push(resetHostedActorTaskState(contextKey, reason, {
      removeState: true,
    }));
  }
  await Promise.all(resets);
}

export function stopAllHostedActorTasks(reason: string): void {
  for (const contextKey of Array.from(hostedActorTaskStates.keys())) {
    // Fire-and-forget; the returned promise resolves asynchronously after
    // the browser acks the stop, but this function is callable from sync
    // unload handlers where awaiting is not possible.
    void stopHostedActorTaskStateIfNeeded(contextKey, reason, {
      removeState: true,
    });
  }
  hostedActorTaskStates.clear();
  activeHostedActorContextKey = null;
}

// Session-scoped stop: drains pending actions for the session's tracked task
// and fires glicBrowserStopActorTask for it. Used by the Stop hook in nexus_web
// to ensure `browser_*` actor tasks don't leak across turns. Returns whether
// an active task was found to stop — callers can log / surface this.
export async function stopHostedActorTaskForSession(
    executionContext: HostedToolExecutionContext,
    reason: string): Promise<{stopped: boolean}> {
  const contextKey = getHostedActorContextKey(executionContext);
  const state = hostedActorTaskStates.get(contextKey);
  const willStop = !!(state && state.taskId !== null && state.apiHost);
  await stopHostedActorTaskStateIfNeeded(contextKey, reason, {
    removeState: true,
  });
  return {stopped: willStop};
}

// =========================================================================
//  HostedToolHost factory
// =========================================================================

export function createHostedToolHost(
    apiHost: any,
    executionContext: HostedToolExecutionContext): HostedToolHost {
  const contextKey = getHostedActorContextKey(executionContext);
  const state = getHostedActorTaskState(contextKey);
  if (state.apiHost && state.apiHost !== apiHost) {
    // Fire-and-forget; clearHostedActorTaskState inside the stop path runs
    // synchronously so setting state.apiHost below is safe.
    void stopHostedActorTaskStateIfNeeded(contextKey, 'apiHost_changed');
  }
  state.apiHost = apiHost;

  return {
    async createTask(title?: string): Promise<number> {
      if (activeHostedActorContextKey &&
          activeHostedActorContextKey !== contextKey) {
        await resetOtherHostedActorTasks(
            contextKey, `context_switch:${contextKey}`);
      }

      if (state.taskId !== null) {
        if (NEXUS_DEBUG) {
          nexusTrace('reusing hosted actor task', {
            taskId: state.taskId, contextKey, executionContext,
          });
        }
        return state.taskId;
      }

      if (state.createTaskPromise) {
        return state.createTaskPromise;
      }

      state.createTaskPromise = (async (): Promise<number> => {
        try {
          const taskId = await requestHostedActorTaskId(apiHost, title);
          state.taskId = taskId;
          state.apiHost = apiHost;
          activeHostedActorContextKey = contextKey;
          if (NEXUS_DEBUG) {
            nexusTrace('created hosted actor task', {
              taskId, contextKey, executionContext,
            });
          }
          return taskId;
        } catch (error: unknown) {
          const {reasonType, reason} = readErrorReason(error);
          if (NEXUS_DEBUG) {
            nexusTrace('createTask failed', {
              reasonType, reason,
              message: error instanceof Error ? error.message : String(error),
              cachedTaskId: state.taskId, contextKey, executionContext,
            });
          }
          if (reasonType === 'createTask' &&
              reason === CREATE_TASK_ERROR_EXISTING_ACTIVE_TASK &&
              state.taskId !== null) {
            if (NEXUS_DEBUG) {
              nexusTrace('returning cached task after ExistingActiveTask', {
                taskId: state.taskId, contextKey,
              });
            }
            return state.taskId;
          }
          if (reasonType === 'createTask' &&
              reason === CREATE_TASK_ERROR_EXISTING_ACTIVE_TASK) {
            const {activeTaskId, lastTaskId} = readApiHostActorTaskIds(apiHost);
            const hostTaskId = activeTaskId ?? lastTaskId;
            if (isPositiveInteger(hostTaskId)) {
              state.taskId = hostTaskId;
              state.apiHost = apiHost;
              state.registry.clear();
              dropOtherHostedActorTaskStates(contextKey);
              activeHostedActorContextKey = contextKey;
              if (NEXUS_DEBUG) {
                nexusTrace('adopted existing hosted actor task from host status', {
                  taskId: hostTaskId, contextKey, executionContext,
                });
              }
              return hostTaskId;
            }

            const stoppedUnknownTask =
                await stopUnknownHostedActorTask(apiHost, contextKey);
            if (stoppedUnknownTask) {
              resetHostedActorTaskTracking(contextKey);
              const retriedTaskId =
                  await requestHostedActorTaskId(apiHost, title);
              state.taskId = retriedTaskId;
              state.apiHost = apiHost;
              activeHostedActorContextKey = contextKey;
              if (NEXUS_DEBUG) {
                nexusTrace('recovered stale hosted actor task by stop+retry', {
                  taskId: retriedTaskId, contextKey, executionContext,
                });
              }
              return retriedTaskId;
            }

            throw new Error(
                'createTask: Chromium already has an active actor task for this panel, and Nexus could not recover it automatically. Close and reopen the panel once to clear stale state.');
          }
          throw error;
        } finally {
          state.createTaskPromise = null;
        }
      })();

      return state.createTaskPromise;
    },
    async resetTask(reason?: string): Promise<void> {
      await resetHostedActorTaskState(contextKey, reason ?? 'hosted_tool_reset');
    },
    performActions(
        actions: ArrayBuffer,
        options?: {signal?: AbortSignal}): Promise<ArrayBuffer> {
      // Register this call in the contextKey's pending set so concurrent
      // stop/reset paths can drain it before telling the browser to stop the
      // task. The entry is scoped to the originating state even if state is
      // later replaced — we snapshot the state reference at registration.
      const pendingState = state;
      const signal = options?.signal;
      const startedAt = Date.now();
      if (NEXUS_DEBUG) {
        nexusTrace('performActions dispatch', {
          contextKey,
          taskId: pendingState.taskId,
          bytes: actions.byteLength,
          pendingCount: pendingState.pendingActions.size,
        });
      }
      const work = async (): Promise<ArrayBuffer> => {
        const response =
            await callApiHostRequest(apiHost, 'glicBrowserPerformActions', {
              actions,
            });
        if (!(response?.actionsResult instanceof ArrayBuffer)) {
          throw new Error('performActions: failed');
        }
        return response.actionsResult;
      };
      const inFlight = work();
      pendingState.pendingActions.add(inFlight);
      void inFlight.then(
          resultBuffer => {
            if (NEXUS_DEBUG) {
              nexusTrace('performActions settled', {
                contextKey,
                elapsedMs: Date.now() - startedAt,
                resultBytes: resultBuffer.byteLength,
              });
            }
          },
          err => {
            if (NEXUS_DEBUG) {
              nexusTrace('performActions native error', {
                contextKey,
                elapsedMs: Date.now() - startedAt,
                message: err instanceof Error ? err.message : String(err),
              });
            }
          },
      );
      void inFlight.finally(() => {
        pendingState.pendingActions.delete(inFlight);
      });

      if (!signal) {
        return inFlight;
      }

      // Signal-aware wrapper: on abort we detach the caller-visible promise
      // immediately so the JS-side caller can fall through to its timeout
      // result. CRITICAL: we must NOT remove the entry from pendingActions
      // here — the underlying Mojo call is still running on the native side
      // and `drainPendingActions` (called from resetHostedActorTaskState
      // before glicBrowserStopActorTask) MUST keep waiting for it. If we
      // delete-on-abort, the drain returns immediately, the stop fires
      // while the response callback is still bound, and the native
      // GlicActorTaskManager::NotifyActorTaskStateChanged path calls
      // reload_observer_.reset() while a pending Mojo PerformActions
      // response is still in flight — tripping the
      // ProxyToResponder::OnIsConnectedComplete DCHECK in glic.mojom.cc and
      // crashing the browser process. The `inFlight.finally` above is the
      // only place that should remove the entry, and only after the
      // native callback actually settled (or the bridge transport's 30s
      // synthesized-timeout rejected it).
      return new Promise<ArrayBuffer>((resolve, reject) => {
        let settled = false;
        const onAbort = () => {
          if (settled) return;
          settled = true;
          // Intentionally NOT calling pendingState.pendingActions.delete —
          // see comment above. The finally on `inFlight` clears it.
          signal.removeEventListener('abort', onAbort);
          if (NEXUS_DEBUG) {
            nexusTrace('performActions aborted by caller', {contextKey});
          }
          const reason = signal.reason;
          reject(reason instanceof Error ? reason : new Error('AbortError'));
        };
        if (signal.aborted) {
          queueMicrotask(onAbort);
          return;
        }
        signal.addEventListener('abort', onAbort);
        void inFlight.then(
            value => {
              if (settled) return;
              settled = true;
              signal.removeEventListener('abort', onAbort);
              resolve(value);
            },
            error => {
              if (settled) return;
              settled = true;
              signal.removeEventListener('abort', onAbort);
              reject(error);
            },
        );
      });
    },
    listSkillPreviews(): Promise<any[]> {
      return Promise.resolve(normalizeSkillPreviews(apiHost.latestSkillPreviews));
    },
    async getSkill(id: string): Promise<any | undefined> {
      const response = await callApiHostRequest(apiHost, 'glicBrowserGetSkill', {
        id,
      });
      return response?.skill;
    },
    async createSkill(request): Promise<{ok: true; modalOpened?: boolean}> {
      const response =
          await callApiHostRequest(apiHost, 'glicBrowserCreateSkill', {
            request: {
              ...request,
              source: 2,
            },
          });
      return {
        ok: true,
        modalOpened: response?.modalOpened === true,
      };
    },
    async showManageSkillsUi(): Promise<{ok: true}> {
      await callApiHostRequest(apiHost, 'glicBrowserShowManageSkillsUi');
      return {ok: true};
    },

    // Best-effort cancel of any in-flight native PerformActions call. Used by
    // the JS-side observation-timeout guard to unblock the actor task queue
    // when a post-action APC fetch stalls (error pages that never fire FCP,
    // etc.). Errors are swallowed — the caller races this against its own
    // deadline and falls through to a skipAsyncObservation retry regardless.
    async cancelCurrentActions(): Promise<void> {
      const taskId = state.taskId;
      if (!isPositiveInteger(taskId)) {
        return;
      }
      try {
        await callApiHostRequest(
            apiHost, 'glicBrowserCancelActions', {taskId});
        if (NEXUS_DEBUG) {
          nexusTrace('cancelCurrentActions sent', {contextKey, taskId});
        }
      } catch (error: unknown) {
        if (NEXUS_DEBUG) {
          nexusTrace('cancelCurrentActions failed', {
            contextKey, taskId,
            message: error instanceof Error ? error.message : String(error),
          });
        }
      }
    },

    // Best-effort lightweight fetch of the focused tab's URL/title, bypassing
    // the actor pipeline. Used after an observation timeout so we can still
    // tell the model what page we were on. Options are all-false — only the
    // synchronously-available TabData (URL, title) is returned, which avoids
    // the APC/innerText/screenshot fetch paths that might themselves be hung.
    async getFocusedTabMetadata():
        Promise<{tab_id?: number, url?: string, title?: string}|undefined> {
      try {
        const response = await callApiHostRequest(
            apiHost, 'glicBrowserGetContextFromFocusedTab', {options: {}});
        const tabData = response?.tabContextResult?.tabData;
        if (!tabData || typeof tabData !== 'object') {
          return undefined;
        }
        const td = tabData as Record<string, unknown>;
        const rawTabId = td['tabId'];
        // TabId is a stringified integer on the upstream wire type.
        let tabIdNumber: number|undefined;
        if (typeof rawTabId === 'string' && rawTabId.trim()) {
          const parsed = Number(rawTabId);
          if (Number.isInteger(parsed) && parsed > 0) {
            tabIdNumber = parsed;
          }
        } else if (isPositiveInteger(rawTabId)) {
          tabIdNumber = rawTabId;
        }
        const url = typeof td['url'] === 'string' ? td['url'] : undefined;
        const title =
            typeof td['title'] === 'string' && td['title'] ? td['title'] : undefined;
        if (tabIdNumber === undefined && !url && !title) {
          return undefined;
        }
        return {tab_id: tabIdNumber, url, title};
      } catch (error: unknown) {
        if (NEXUS_DEBUG) {
          nexusTrace('getFocusedTabMetadata failed', {
            contextKey,
            message: error instanceof Error ? error.message : String(error),
          });
        }
        return undefined;
      }
    },
  };
}
