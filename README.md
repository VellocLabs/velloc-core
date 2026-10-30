# velloc-core

The native half of [Velloc](https://github.com/VellocLabs/velloc-bootstrap):
the C++ that turns a Chromium build into the platform the Velloc runtime runs
on.

It lives at `src/custom_browser/` inside a Chromium checkout and is compiled
into the browser binary. It is not a standalone library.

> **Do you need this repository?** To use Velloc, install it from the
> [Releases page](https://github.com/VellocLabs/velloc-bootstrap/releases/latest).
> To change the agent — its loop, tools, prompts or UI — work on
> [velloc-runtime](https://github.com/VellocLabs/velloc-runtime) against an
> installed client; no C++ build is involved. Build from here only to change
> the native side: capabilities, LLM transport, the bridge, the installer.

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

## Build the Velloc client

This repository builds only inside the full workspace:
[velloc-bootstrap](https://github.com/VellocLabs/velloc-bootstrap) holds the
`.gclient`, the build arguments and the scripts; it checks out
[velloc-chromium](https://github.com/VellocLabs/velloc-chromium) as `src/` and
this repository as `src/custom_browser/`.

```
velloc/                     velloc-bootstrap   scripts, args/, .gclient
├── depot_tools/            installed by bootstrap.sh
├── nexus_web/              velloc-runtime     optional: your runtime checkout
└── src/                    velloc-chromium    branch custom/main
    ├── custom_browser/     velloc-core        this repository
    └── out/Debug/          build output
```

### Prerequisites

Windows 10 or 11, Git, Python 3, Bash (Git Bash), Visual Studio 2022 with the
*Desktop development with C++* workload and the Windows SDK, and **100 GB+ of
free disk** on an NTFS drive. The first build takes hours; later builds take
minutes.

### First build

```bash
git clone https://github.com/VellocLabs/velloc-bootstrap.git velloc
cd velloc
./bootstrap.sh       # 1) Install depot_tools, then 3) Bootstrap & Sync
./build.sh           # Build Debug
```

On Windows PowerShell, `.\bootstrap.bat` and `.\build.bat` open the same
menus.

The client lands at `src/out/Debug/velloc.exe`. A debug client loads its
runtime from `http://localhost:5173`, so start one first:

```bash
git clone https://github.com/VellocLabs/velloc-runtime.git nexus_web
cd nexus_web && npm install && npm run dev
```

To build exactly a released client, choose **5) Restore release snapshot** in
`bootstrap.sh`: it checks every repository out to the commits recorded in
`release_manifest.json`.

### Every build after that

```bash
cd src && autoninja -C out/Debug chrome -j 15
```

A targeted change rebuilds in 1–3 minutes.

> **Keep builds incremental.** Running `gn gen` by hand, deleting `src/out/*`
> or editing `args.gn` forces a full rebuild of several hours. Change build
> arguments in `args/*.gn` and build through `build.sh`.
>
> **Judge a build by `chrome.dll`'s timestamp**, not `velloc.exe`'s: the
> `.exe` is a small launcher and is often not relinked.

### `bootstrap.sh` menu

| Option | Does |
|---|---|
| 1) Install depot_tools | Clones `depot_tools` into `./depot_tools`. Run this first. |
| 2) Fast sync | Shallow-fetches `src/` and `src/custom_browser/`, then a shallow `gclient sync`. Smallest download, no history. |
| 3) Bootstrap & Sync | Clones `src/` and `src/custom_browser/` if missing, sets remotes and branches, then a shallow forced `gclient sync`. **The normal first run.** |
| 4) Bootstrap (force) & Sync | Deletes `src/` and clones it again. Resets a broken workspace. |
| 5) Restore release snapshot | Checks every repository out to the commits in `release_manifest.json`. |

Environment overrides: `SRC_URL`, `SRC_BRANCH` (default `custom/main`),
`SRC_REVISION`, `CUSTOM_BROWSER_URL`, `CUSTOM_BROWSER_BRANCH` (default
`main`), `CUSTOM_BROWSER_REVISION`. Point `CUSTOM_BROWSER_URL` at your fork to
build your own changes. The same steps without the menu:
`python scripts/bootstrap_cli.py install-tools | fast-sync | bootstrap | sync |
restore`.

### `build.sh` menu

One **Build …** entry per file in `args/`:

| Entry | Output | For |
|---|---|---|
| Build Debug | `src/out/Debug` | development; loads the runtime from `localhost:5173` |
| Build Release | `src/out/Release` | an optimised local build |
| Build Velloc | `src/out/Velloc` | the production configuration |

**Build mini_installer** asks for a configuration and builds Chromium's
installer from it; **Package Velloc NSIS** builds `VellocSetup.exe`, and
**Reinstall Velloc NSIS** also installs it. A new `*.gn` file in `args/`
becomes a new menu entry. `build.sh` also
fetches `gn` when it is missing and pre-starts the compiler cache, so build
through it rather than with your own invocation.

### Testing a change

Unit tests sit next to the code as `*_unittest.cc`. Most build into
`custom_browser_nexus_unittests`; tests that need a browser process
(profiles, resources) build into `custom_browser_nexus_browser_unittests`:

```bash
cd src && autoninja -C out/Debug custom_browser_nexus_unittests -j 15
out/Debug/custom_browser_nexus_unittests.exe --gtest_filter='ProviderConfig*'
```

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
The two files under `utility/importer/` headed The Brave Authors are under
the Mozilla Public License 2.0, as their headers state.
The bundled ripgrep binary is under the MIT licence; see
[installer/bundled/rg-LICENSE-MIT](installer/bundled/rg-LICENSE-MIT).
The bundled console host (`conpty.dll`, `OpenConsole.exe`, from Windows
Terminal's ConPTY package) is under the MIT licence; see
[installer/bundled/conpty-LICENSE-MIT](installer/bundled/conpty-LICENSE-MIT).
