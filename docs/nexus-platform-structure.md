# Nexus Platform Structure

## Direct Bridge Runtime

Every runtime capability call (`fs.*`, `model.*`, `ui.*`, `process.*`,
`glic.*`) goes through a single browser-owned port bridge. The steady-state
path is:

`SharedWorker -> worker bridge port -> NexusBridgePortService -> capability adapter`

`glic.*` is served by `NexusGlicBridgeAdapter`
(`browser/nexus/glic/nexus_glic_bridge_adapter.cc`) which calls
`actor::ActorKeyedService::CreateTask` / `PerformActions` / `StopTask`
directly. There is no chrome://glic page anymore — the legacy
`resources/patch_webui/glic/` resource bundle and the
`browser/nexus/view/nexus_view_container.{h,cc}` side-panel `views::WebView`
have been deleted.

## Bootstrap Path

The CustomBrowserWebUI page (`chrome://custom.top-chrome`) hosts the
nexus_web guest in a `<webview id="nexusPane">` directly:

1. `resources/browser_webui/pages/nexus/nexus_view.ts` renders
   `<webview id="nexusPane">` and calls `bootstrapNexusGuest()` from
   `nexus_runtime_bootstrap.ts` in `firstUpdated()`.
2. The bootstrap controller sets `webview.src` to the resolved guest URL
   (`loadTimeData.getString('customBrowserNexusGuestUrl')`), then runs the
   `nexus-init` ↔ `nexus-init-ack` handshake against the guest.
3. `NexusBridgeBootstrapHost` receives `GuestReady()` over the tiny
   `NexusBridgeBootstrap` Mojo interface (bound by
   `features/patch_webui/custom_browser_hooks.cc`).
4. The browser creates two direct bridge port pairs:
   - `pagePort` for page-thread browser calls
   - `workerPort` for SharedWorker runtime calls
5. The bootstrap controller forwards those ports into the guest webview.
6. The guest forwards `workerPort` into the SharedWorker.

After that, the page-thread is off the hot path for every capability —
`fs.*` / `model.*` / `ui.*` / `process.*` / `glic.*` all run over the
SharedWorker direct port.

The bootstrap message now also carries a browser-owned host manifest from
`browser/nexus/nexus_host_manifest.cc`:

- `hostKind`
- `capabilities`

The guest normalizes that manifest in `openclaw-web` and uses it as the
authoritative browser capability surface for Nexus mode.

Tool names and slash commands are not owned by `custom_browser` today. Those
remain defined in `openclaw-web`, and the browser does not duplicate them in
the Nexus manifest.

## Port Transport

`NexusBridgePortService` no longer uses the string-only `blink::WebMessagePort`
wrapper. Browser-side direct ports use `NexusMessagePortEndpoint`, which is
built on:

- `blink::MessagePortDescriptor`
- `mojo::Connector`
- `blink::TransferableMessage`
- `blink::WebMessagePayload`

That allows:

- string control messages for the bridge envelope
- transferred ports for per-request binary channels
- `ArrayBuffer` payloads for file bytes

## Bridge Contract

Control messages still use the existing bridge envelope:

- request: `hostBridge: true`, `direction: "request"`
- response: `hostBridge: true`, `direction: "response"`

Transport modes:

- JSON control only: `fs.cwd`, `fs.stat`, `fs.readdir`, `fs.exists`,
  `fs.mkdir`, `fs.unlink`, `model.catalog`, `model.call`,
  `model.streamStart`, `model.streamRead`, `glic.createTask`,
  `glic.stopTask`, `glic.cancelActions`,
  `glic.getContextFromFocusedTab`, `glic.actorGetStatus`,
  `glic.getSkill`, `glic.createSkill`, `glic.showManageSkillsUi`,
  `glic.getLatestSkillPreviews`
- response data port: `fs.readFile`, `fs.readFileBytes`, `fs.readRange`,
  `glic.performActions` (carries the binary `ActionsResult` proto)
- request data port: `fs.writeFile`, `fs.appendFile`

Bulk filesystem bytes now move as raw `ArrayBuffer`, not JSON byte arrays and
not base64.

## Browser Services

- `NexusFsHost` is now a plain async service class. It no longer implements a
  Mojo interface.
- `NexusLlmHost` is now a plain async service class. It no longer implements a
  Mojo interface.
- The filesystem Mojo definition has been removed.
- The LLM path still uses `nexus_llm.mojom` data structs internally as the
  current DTO layer. That is a separate cleanup from the transport migration.

## Production Tool Surface

Nexus production no longer exposes the mock GLIC tool set. Those tools remain
demo/test-only in `openclaw-web` under the `glic-demo` preset.

In Nexus mode, production tool exposure is driven by the browser manifest
instead:

- supported bridge capabilities come from the browser
- model-visible tool definitions still come from `openclaw-web`
- slash commands still come from `openclaw-web`

This is intentional: the current Chromium GLIC integration exposes browser
APIs, contextual skills, and `supportedTools` metadata through the injected
Glic API, but it does not currently export a ready-to-use openclaw tool catalog.

The production `openclaw-web` GLIC surface now wraps two kinds of host-backed
capabilities:

- direct JSON-friendly host APIs such as panel context, tab context, tab
  creation, activation, close panel, and skills
- high-level actor tools built on top of Chromium `createTask` /
  `performActions`, currently for actor status, navigation, coordinate click,
  and stop
