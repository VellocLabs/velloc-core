// GENERATED — DO NOT EDIT.
//
// Mirror of nexus_web/src/browser/runtimeProtocol/route.ts, copied by
// nexus_web/scripts/sync_runtime_protocol_mirror.py so the outer WebUI page can
// speak the UI <-> Runtime protocol without a hand-written envelope. Edit the
// nexus_web source and re-run the script; `--check` fails when this file and
// its source disagree (nexus_web `protocol-mirror.test.ts`).

/**
 * UI ⇄ Runtime protocol — route addressing.
 *
 * Every request names a `cap.act` pair. Capability names are the same ones
 * the C++ bridge uses today (`ui`, `appearance`, `session`, …) so the 41
 * direct-bridge call sites in `nexusBridgeActions.ts` migrate by swapping the
 * transport, not the names. Compared with the direct bridge's
 * `BrowserBridgeRoute` there is no `namespace` field: this protocol has one
 * namespace, and the hub decides per route whether a handler is local to the
 * runtime or a proxy to the host.
 *
 * Two forms, one meaning: in code a route is the `Route` object; on the wire
 * it is the `RouteKey` string `"cap.act"` (twenty bytes less per frame, and
 * it is what logs and registries key on anyway). `routeKey` / `parseRouteKey`
 * convert, and `parseRouteKey` is strict — exactly two identifier-shaped
 * segments — so the string cannot smuggle a third segment or a space.
 */

export type Route = {
  readonly cap: string
  readonly act: string
}

/** The wire form of a route: `cap.act`. */
export type RouteKey = `${string}.${string}`

/** One route segment: identifier-shaped, so `routeKey` is unambiguous. */
const ROUTE_SEGMENT = /^[A-Za-z][A-Za-z0-9_]*$/

export function isRouteSegment(value: unknown): value is string {
  return typeof value === 'string' && ROUTE_SEGMENT.test(value)
}

export function route(cap: string, act: string): Route {
  return { cap, act }
}

/** `session.submit` — the wire form, and the form registries and logs key on. */
export function routeKey(r: Route): RouteKey {
  return `${r.cap}.${r.act}`
}

/** Inverse of `routeKey`; `null` for anything that is not exactly two segments. */
export function parseRouteKey(key: string): Route | null {
  const dot = key.indexOf('.')
  if (dot < 0) return null
  const cap = key.slice(0, dot)
  const act = key.slice(dot + 1)
  if (!isRouteSegment(cap) || !isRouteSegment(act)) return null
  return { cap, act }
}

/** A well-formed wire route: exactly `cap.act` with identifier-shaped segments. */
export function isRouteKey(value: unknown): value is RouteKey {
  return typeof value === 'string' && parseRouteKey(value) !== null
}

export function isRoute(value: unknown): value is Route {
  if (!value || typeof value !== 'object') return false
  const v = value as Record<string, unknown>
  return isRouteSegment(v['cap']) && isRouteSegment(v['act'])
}
