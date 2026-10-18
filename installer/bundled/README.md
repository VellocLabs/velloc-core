# Bundled binaries

Third-party executables packed into the mini_installer archive. Each is
listed in `//chrome/installer/mini_installer/chrome.release` under
`%(VersionDir)s`, so Chromium's `setup.exe` installs it into
`Application\<version>\` next to `chrome.dll` (and removes it with that
version on upgrade / uninstall). At runtime
`custom_browser::GetBundledBinaryPath()` finds `rg.exe` there, so a packaged
build never downloads it into `~/.velloc/bin`;
`custom_browser::GetBundledConsoleHostPath()` finds the console host pair.

These are the **upstream** bytes, unmodified. `installer/BUILD.gn`
(`bundled_binaries`, `console_host`) copies each into the out dir;
`build.sh package`'s payload signing signs that copy like every other `.exe`
and `.dll` the manifest lists, before the archive is packed, and leaves a
file Microsoft already signed as it is — the file here is never modified.

| File | Upstream | SHA-256 (of this file) |
|---|---|---|
| `rg.exe` | ripgrep 14.1.1, `ripgrep-14.1.1-x86_64-pc-windows-msvc.zip` member `rg.exe` (archive SHA-256 `d0f534024c42afd6cb4d38907c25cd2b249b79bbe6cc1dbee8e3e37c2b6e25a1`) | `f162b54de2adfc72d78adb1dbada2dedda111ae0a5e2f6e9500f4f909664c5d2` |
| `conpty.dll` | NuGet `Microsoft.Windows.Console.ConPTY` 1.25.260930003 member `runtimes/win-x64/native/conpty.dll` (package SHA-256 `02b07b349af66d801159bdf9e440d4a1ce78bb951f37fc8609731665afdae7ee`) | `feeef341d891643c62d30b6b07800bc70f0bc148f44cb8c3bee84aa557ae805a` |
| `OpenConsole.exe` | the same package, member `build/native/runtimes/x64/OpenConsole.exe` | `3d66b23d0a71bb8eed2b77edc8b9df9bf54ce6c8fb74c863a30e760997f80586` |

ripgrep is dual-licensed MIT / Unlicense — see `rg-LICENSE-MIT`.
The console host (Windows Terminal's ConPTY package) is MIT — see
`conpty-LICENSE-MIT`. Both of its files are Authenticode-signed by Microsoft
as shipped (file version 1.25.2609.30003), x64 only.

`conpty.dll` and `OpenConsole.exe` must stay in one directory: the DLL
starts the host from its own. Unlike `rg.exe`, they are also copied into a
dev build's out dir (`console_host` is a `data_deps` of the loader), so the
terminal runs on the same host a packaged build uses; the OS console host is
the fallback (`--velloc-console-host=system`). (F-31)

Bumping a version: replace the file(s) and update the hashes here. Neither
is in the managed-binary download registry
(`browser/nexus/binaries/nexus_binary_registry.cc`), so nothing else pins
them.
