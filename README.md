# velloc-core

The native half of [Velloc Code](https://github.com/VellocLabs/velloc-runtime):
the C++ that turns a Chromium build into the platform the Velloc runtime runs
on.

It lives at `src/custom_browser/` inside a Chromium checkout and is compiled
into the browser binary. It is not a standalone library.

## What it provides

The runtime decides; this repository executes. Each capability is declared in
an `*.api.json` file, the host stubs are generated from it, and the runtime
reaches it only through the bridge.

| Area | Where | What it does |
|---|---|---|
| Bridge | `browser/nexus/` | Bootstraps the runtime's guest surface and carries every call over a message port |
| Capabilities | `browser/nexus/api/` | Files, shell, exec, search, browser control, model transport, secrets, session, settings, UI |
| Tools | `browser/nexus/tools/` | Native bodies of the file, process and browser tools, with per-session state |
| Shell | `utility/` | Bash and PowerShell in a sandboxed utility process |
| Remote access | `browser/nexus/remote_access/` | Lets another device connect to a running client |
| Hooks into Chromium | `chromium_hooks/`, `chromium_overrides/`, `features/` | The small, guarded points where the product plugs into the browser |
| Strings | `resources/` | User-visible text, translated into every supported locale |
| Installer | `installer/` | The Windows NSIS installer and the files it bundles |

## The bridge contract

`VERSION` holds `NEXUS_INTERFACE_VERSION`, the one number that matches a
client to the runtimes it can load. A change to a declaration, to the wire
types or to the bootstrap message bumps it. A client loads a hosted runtime
from the path for its own version, so a runtime deploy can never change the
contract under an installed client.

## Build

This repository builds only as part of the full workspace. Bootstrap, sync and
incremental builds are described in the workspace repository:
[velloc-bootstrap](https://github.com/VellocLabs/velloc-bootstrap#readme).

Working on the runtime alone does not need this repository at all: an
installed client loads a runtime from a local dev server.

## Rules for changes

- **All custom C++ lives here.** Upstream Chromium code is not edited. Where a
  hook into upstream is unavoidable, it is minimal and guarded by
  `ENABLE_CUSTOM_BROWSER`.
- **Every feature and fix ships with tests**, next to the code as
  `*_unittest.cc`.
- **No hard-coded text a user can read.** User-visible strings go in
  `resources/custom_browser_strings.grd`, English first, translated in the
  same change.
- **History stays linear.** Rebase onto `main`; no merge commits.

## License

Velloc's own code is licensed under the Apache License 2.0; see
[LICENSE](LICENSE).

Files whose header names The Chromium Authors are Chromium code and remain
under Chromium's BSD-style licence; see [LICENSE.chromium](LICENSE.chromium).
The bundled ripgrep binary is under the MIT licence; see
[installer/bundled/rg-LICENSE-MIT](installer/bundled/rg-LICENSE-MIT).
