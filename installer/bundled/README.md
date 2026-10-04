# Bundled binaries

Third-party executables packed into the mini_installer archive. Each is
listed in `//chrome/installer/mini_installer/chrome.release` under
`%(VersionDir)s`, so Chromium's `setup.exe` installs it into
`Application\<version>\` next to `chrome.dll` (and removes it with that
version on upgrade / uninstall). At runtime
`custom_browser::GetBundledBinaryPath()` finds it there, so a packaged build
never downloads it into `~/.velloc/bin`.

These are the **unsigned upstream** bytes. `installer/BUILD.gn`
(`bundled_binaries`) copies each into the out dir; `build.sh package`'s
payload signing signs that copy like every other `.exe` the manifest lists,
before the archive is packed — the file here is never modified.

| File | Upstream | SHA-256 (of this file) |
|---|---|---|
| `rg.exe` | ripgrep 14.1.1, `ripgrep-14.1.1-x86_64-pc-windows-msvc.zip` member `rg.exe` (archive SHA-256 `d0f534024c42afd6cb4d38907c25cd2b249b79bbe6cc1dbee8e3e37c2b6e25a1`) | `f162b54de2adfc72d78adb1dbada2dedda111ae0a5e2f6e9500f4f909664c5d2` |

ripgrep is dual-licensed MIT / Unlicense — see `rg-LICENSE-MIT`.

Bumping the version: replace the file and update both hashes here. rg is
not in the managed-binary download registry
(`browser/nexus/binaries/nexus_binary_registry.cc`), so nothing else pins it.
