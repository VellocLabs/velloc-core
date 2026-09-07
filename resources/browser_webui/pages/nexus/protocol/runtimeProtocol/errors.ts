// GENERATED — DO NOT EDIT.
//
// Mirror of nexus_web/src/browser/runtimeProtocol/errors.ts, copied by
// nexus_web/scripts/sync_runtime_protocol_mirror.py so the outer WebUI page can
// speak the UI <-> Runtime protocol without a hand-written envelope. Edit the
// nexus_web source and re-run the script; `--check` fails when this file and
// its source disagree (nexus_web `protocol-mirror.test.ts`).

/**
 * UI ⇄ Runtime protocol — the closed error-code set.
 *
 * `WireError.code` is a closed union on purpose: UI copy is looked up by code
 * in the message catalog, never rendered from `message` (which is English,
 * for logs). `retryable` is derived from the code, not chosen per call, so
 * two handlers cannot disagree about whether the same failure is worth a
 * retry.
 */

export const ERROR_CODES = [
  'unauthorized',
  'forbidden_scope',
  'unknown_route',
  'bad_payload',
  'not_found',
  'conflict',
  'stale',
  'timeout',
  'cancelled',
  'host_unavailable',
  'internal',
] as const

export type ErrorCode = (typeof ERROR_CODES)[number]

export type WireError = {
  code: ErrorCode
  /** English, for logs and diagnostics. UI text comes from the catalog keyed by `code`. */
  message: string
  data?: unknown
  /** true = repeating the same request is meaningful (timeout, host busy). */
  retryable: boolean
}

/** Codes for which the identical request may be retried as-is. */
export const RETRYABLE_ERROR_CODES: ReadonlySet<ErrorCode> = new Set<ErrorCode>([
  'timeout',
  'host_unavailable',
])

export function isErrorCode(value: unknown): value is ErrorCode {
  return typeof value === 'string' && (ERROR_CODES as readonly string[]).includes(value)
}

export function wireError(code: ErrorCode, message: string, data?: unknown): WireError {
  const error: WireError = { code, message, retryable: RETRYABLE_ERROR_CODES.has(code) }
  if (data !== undefined) {
    error.data = data
  }
  return error
}

/** Structural guard; with `code` it also narrows to that one code. */
export function isWireError(value: unknown, code?: ErrorCode): value is WireError {
  if (!value || typeof value !== 'object') return false
  const v = value as Record<string, unknown>
  if (!isErrorCode(v['code'])) return false
  if (typeof v['message'] !== 'string') return false
  if (typeof v['retryable'] !== 'boolean') return false
  return code === undefined || v['code'] === code
}
