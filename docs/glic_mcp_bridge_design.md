# GLIC Actor -> MCP Bridge (Minimal Design)

## Goal
Expose the existing GLIC actor automation backend as MCP tools so external LLM agents can use it.

This design keeps GLIC/actor execution in Chromium and adds a thin MCP adapter.

## Current GLIC Actor Flow (Already in Code)
1. Web client calls `createTask` / `performActions` in GLIC API client:
   - `chrome/browser/resources/glic/glic_api_impl/client/glic_api_client.ts`
2. Host bridge forwards to Mojo handler:
   - `chrome/browser/resources/glic/glic_api_impl/host/host_from_client.ts`
3. Browser handler routes to GLIC keyed service:
   - `chrome/browser/glic/host/glic_page_handler.cc`
   - `chrome/browser/glic/public/glic_keyed_service.cc`
4. Actor task manager executes actions:
   - `chrome/browser/glic/actor/glic_actor_task_manager.cc`
5. Actor execution engine + tool controller + renderer tool executor run actions:
   - `chrome/browser/actor/execution_engine.cc`
   - `chrome/browser/actor/tools/tool_controller.cc`
   - `chrome/renderer/actor/tool_executor.cc`

## Recommended Minimal Architecture
### Components
1. `Browser-side GlicMcpBridge` (new, in Chromium process)
   - Wraps `GlicKeyedService` actor calls.
   - Exposes a localhost-only RPC endpoint (or named pipe) when explicitly enabled.
2. `MCP Adapter Server` (new, separate process)
   - Implements MCP transport (`stdio` first).
   - Translates MCP tool calls to bridge RPC.

### Why this split
- Keeps policy/safety/task semantics inside browser code.
- MCP server stays thin and easy to evolve.
- Avoids duplicating actor logic in Node/Python.

## Minimal Scope (v0)
Expose only low-level actor operations plus context read:
- `glic_create_task`
- `glic_perform_actions_proto`
- `glic_get_context_for_actor_from_tab`
- `glic_cancel_actions`
- `glic_stop_actor_task`
- `glic_pause_actor_task`
- `glic_resume_actor_task`

For v0, `performActions` uses base64-encoded serialized proto to avoid implementing a JSON->Actions compiler in the first cut.

## Suggested Browser Bridge API (internal RPC)
Methods mirror existing handler signatures:
- `CreateTask(task_options?) -> { task_id }`
- `PerformActionsProto(actions_proto_base64) -> { actions_result_proto_base64 }`
- `GetContextForActorFromTab(tab_id, options) -> { tab_context }`
- `CancelActions(task_id) -> { result }`
- `StopActorTask(task_id, stop_reason) -> { ok }`
- `PauseActorTask(task_id, pause_reason, tab_id?) -> { ok }`
- `ResumeActorTask(task_id, context_options) -> { result }`

## MCP Tool Contract
See `docs/glic_mcp_tools_minimal.json` for the concrete minimal schema.

## Security Requirements
Must-have for first release:
1. Disabled by default; enable only with explicit switch (example: `--glic-mcp-bridge`).
2. Bind to loopback only.
3. Require per-session auth token in every bridge request.
4. Keep existing GLIC actor policy checks in place (do not bypass).
5. Add audit logging with task id, tool name, caller id.

Recommended:
1. Per-profile allowlist.
2. Rate limits by caller.
3. Optional user confirmation for high-risk actions.

## Error Model
Return structured errors from bridge and preserve reason codes when possible:
- `invalid_request`
- `unauthorized`
- `task_not_found`
- `invalid_proto`
- `action_failed`
- `internal_error`

In MCP response, include both:
- human-readable message
- machine code (`code`)

## Rollout Plan
1. Add bridge service with feature flag and auth token.
2. Implement 3 core methods first:
   - create task
   - perform actions (proto passthrough)
   - cancel actions
3. Add context and pause/resume/stop methods.
4. Add MCP adapter server (`stdio`) with tool list.
5. Add integration tests:
   - happy path click action
   - invalid proto
   - unauthorized caller
   - tab gone

## Future (v1+)
1. Add semantic tools (`glic_click`, `glic_type`, `glic_navigate`) that compile to Actions proto.
2. Add multi-tab task helpers.
3. Stream intermediate task state updates as MCP notifications.

