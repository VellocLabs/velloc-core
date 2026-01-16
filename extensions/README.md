Custom component extensions live under `custom_browser/extensions` and are
packaged into a separate GRIT pak that is merged into `resources.pak` when
`enable_custom_browser=true`.

Layout
```
custom_browser/extensions/
  extensions.json
  custom_extensions/
    <name>/
      manifest.json
      ...runtime assets...
```

Add a new built-in extension
- Drop the extension files under `custom_browser/extensions/custom_extensions/<name>/`.
- Ensure `manifest.json` includes a `"key"` and a supported `"manifest_version"`.
  - Component extensions derive a stable ID from `"key"`; missing/invalid keys
    are rejected by the allowlist.
  - MV2 is allowed for component extensions; MV3 works too.
- Add an entry to `custom_browser/extensions/extensions.json`:
  - `"name"`: stable identifier used for resource IDs.
  - `"path"`: root path relative to `chrome::DIR_RESOURCES` (recommended:
    `"custom_extensions/<name>"`).
  - `"background_page"`: optional boolean, `true` if the extension uses a
    background page (MV2) or service worker (MV3) (default `false`).

Notes
- Only runtime assets should be placed in the extension directory; the build
  script ignores `.gn`, `.gni`, `.md`, `.py`, `.ts`, `.map`, and dotfiles.
- If the extension needs to load in kiosk mode or other special sessions, add
  custom logic in `custom_browser/extensions/custom_component_extensions.cc`.

Build optimization (esbuild)
- To minify JS/CSS before packaging, add an `"esbuild"` block in
  `custom_browser/extensions/extensions.json`.
- The build stages optimized files under `$root_gen_dir/custom_browser/extensions`
  and packages from there; the source tree is untouched.
- Bundling is opt-in and requires explicit entrypoints. Use `"format": "esm"`
  for module scripts or `"iife"` for classic scripts; bundling can break MV2
  scripts that rely on global ordering.
- If the extension uses absolute-path imports (e.g. `import('/js/foo.js')`),
  set `"external": ["/js/*"]` so esbuild does not try to resolve them.

Example:
```
{
  "name": "uBlock",
  "path": "custom_extensions/uBlock",
  "background_page": true,
  "esbuild": {
    "minify": true,
  }
}
```
