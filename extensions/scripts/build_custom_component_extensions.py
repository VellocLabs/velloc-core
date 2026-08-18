#!/usr/bin/env python3
import argparse
import json
import os
import shutil
import subprocess
import sys
from typing import Dict, List, Optional

SUPPORTED_EXTENSIONS = {".js", ".mjs", ".cjs", ".css"}


def normalize_relative_path(value: str) -> str:
    path = value.replace("\\", "/").strip("/")
    if not path:
        raise ValueError("empty extension path")
    if ".." in path.split("/"):
        raise ValueError(f"invalid path: {value}")
    return path


def collect_files(root: str) -> List[str]:
    files = []
    for base, _, filenames in os.walk(root):
        filenames.sort()
        for filename in filenames:
            full_path = os.path.join(base, filename)
            files.append(os.path.abspath(full_path))
    return files


def collect_assets(root: str) -> List[str]:
    assets = []
    for base, _, filenames in os.walk(root):
        filenames.sort()
        for filename in filenames:
            ext = os.path.splitext(filename)[1].lower()
            if ext not in SUPPORTED_EXTENSIONS:
                continue
            full_path = os.path.join(base, filename)
            rel_path = os.path.relpath(full_path, root).replace(os.sep, "/")
            assets.append(rel_path)
    return assets


def write_depfile(depfile: str, outputs: List[str], inputs: List[str]) -> None:
    if not depfile:
        return
    output_list = " ".join(outputs)
    input_list = " ".join(inputs)
    with open(depfile, "w", encoding="utf-8") as handle:
        handle.write(f"{output_list}: {input_list}\n")


def ensure_bool(value: Optional[object], default: bool, name: str) -> bool:
    if value is None:
        return default
    if isinstance(value, bool):
        return value
    raise ValueError(f"{name} must be a boolean")


def ensure_string(value: Optional[object], name: str) -> Optional[str]:
    if value is None:
        return None
    if isinstance(value, str):
        return value
    raise ValueError(f"{name} must be a string")


def normalize_entrypoints(entrypoints: object, ext_dir: str) -> List[str]:
    if entrypoints is None:
        return []
    if not isinstance(entrypoints, list):
        raise ValueError("entrypoints must be a list")
    ext_dir = os.path.normcase(os.path.abspath(ext_dir))
    results = []
    for item in entrypoints:
        if not isinstance(item, str):
            raise ValueError("entrypoints must contain strings")
        rel_path = normalize_relative_path(item)
        abs_path = os.path.normcase(
            os.path.abspath(os.path.join(ext_dir, rel_path))
        )
        if os.path.commonpath([ext_dir, abs_path]) != ext_dir:
            raise ValueError(f"entrypoint escapes extension root: {item}")
        if not os.path.isfile(abs_path):
            raise ValueError(f"entrypoint not found: {item}")
        results.append(abs_path)
    return results


def build_esbuild_args(
    entrypoints: List[str],
    out_dir: str,
    out_base: str,
    minify: bool,
    bundle: bool,
    external: List[str],
    target: Optional[str],
    platform: Optional[str],
    fmt: Optional[str],
    sourcemap: Optional[object],
) -> List[str]:
    args = list(entrypoints)
    if bundle:
        args.append("--bundle")
    if minify:
        args.append("--minify")
    args.append(f"--outdir={out_dir}")
    args.append(f"--outbase={out_base}")
    args.append("--log-level=warning")
    args.append("--legal-comments=none")
    if target:
        args.append(f"--target={target}")
    if platform:
        args.append(f"--platform={platform}")
    if bundle and fmt:
        args.append(f"--format={fmt}")
    if bundle:
        for pattern in external:
            args.append(f"--external:{pattern}")
    if sourcemap:
        if isinstance(sourcemap, str):
            args.append(f"--sourcemap={sourcemap}")
        else:
            args.append("--sourcemap")
    return args


def run_esbuild(esbuild_path: str, args: List[str]) -> None:
    result = subprocess.run(
        [esbuild_path] + args,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        sys.stderr.write(result.stderr or result.stdout)
        raise RuntimeError("esbuild failed")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--root-dir", required=True)
    parser.add_argument("--out-gen-dir", required=True)
    parser.add_argument("--out-json", required=True)
    parser.add_argument("--esbuild", required=True)
    parser.add_argument("--depfile")
    args = parser.parse_args()

    root_dir = os.path.abspath(args.root_dir)
    out_gen_dir = os.path.abspath(args.out_gen_dir)
    esbuild_path = os.path.abspath(args.esbuild)
    if not os.path.isfile(esbuild_path):
        print(f"esbuild not found: {esbuild_path}", file=sys.stderr)
        return 1

    with open(args.input, "r", encoding="utf-8") as handle:
        data = json.load(handle)

    extensions = data.get("extensions", [])
    if not isinstance(extensions, list):
        print("extensions must be a list", file=sys.stderr)
        return 1

    output_entries: List[Dict[str, object]] = []
    dep_inputs = [os.path.abspath(args.input)]

    for ext in extensions:
        name = ext.get("name")
        if not name or not isinstance(name, str):
            print("extension entry missing valid name", file=sys.stderr)
            return 1

        # Bundled extensions are opt-in: absent means off.
        enabled_by_default = ensure_bool(
            ext.get("enabled_by_default"), False, "enabled_by_default"
        )

        raw_path = ext.get("path", f"custom_extensions/{name}")
        if not isinstance(raw_path, str):
            print(f"invalid path for extension {name}", file=sys.stderr)
            return 1

        rel_path = normalize_relative_path(raw_path)
        ext_dir = os.path.normpath(os.path.join(root_dir, rel_path))
        if not os.path.isdir(ext_dir):
            print(f"extension directory not found: {ext_dir}", file=sys.stderr)
            return 1
        if not os.path.isfile(os.path.join(ext_dir, "manifest.json")):
            print(f"manifest.json missing in {ext_dir}", file=sys.stderr)
            return 1

        esbuild_config = ext.get("esbuild")
        if esbuild_config is None:
            output_entries.append(
                {
                    "name": name,
                    "path": rel_path,
                    "background_page": bool(ext.get("background_page", False)),
                    "enabled_by_default": enabled_by_default,
                    "source_root": "src",
                    "source_path": rel_path,
                }
            )
            continue

        if not isinstance(esbuild_config, dict):
            print(f"esbuild config must be a dict for {name}", file=sys.stderr)
            return 1

        minify = ensure_bool(esbuild_config.get("minify"), True, "minify")
        bundle = ensure_bool(esbuild_config.get("bundle"), False, "bundle")
        external = esbuild_config.get("external", [])
        if not isinstance(external, list) or not all(
            isinstance(item, str) for item in external
        ):
            print("external must be a list of strings", file=sys.stderr)
            return 1
        target = ensure_string(esbuild_config.get("target"), "target")
        platform = ensure_string(esbuild_config.get("platform"), "platform")
        fmt = ensure_string(esbuild_config.get("format"), "format")
        sourcemap = esbuild_config.get("sourcemap")
        if sourcemap is not None and not isinstance(sourcemap, (bool, str)):
            print("sourcemap must be a boolean or string", file=sys.stderr)
            return 1

        entrypoints = normalize_entrypoints(
            esbuild_config.get("entrypoints"), ext_dir
        )
        if bundle and not entrypoints:
            print(f"bundle enabled but no entrypoints provided for {name}", file=sys.stderr)
            return 1

        out_dir = os.path.normpath(os.path.join(out_gen_dir, rel_path))
        if os.path.isdir(out_dir):
            shutil.rmtree(out_dir)
        os.makedirs(os.path.dirname(out_dir), exist_ok=True)
        shutil.copytree(ext_dir, out_dir)

        dep_inputs.extend(collect_files(ext_dir))

        if bundle:
            bundle_args = build_esbuild_args(
                entrypoints=entrypoints,
                out_dir=out_dir,
                out_base=ext_dir,
                minify=minify,
                bundle=True,
                external=external,
                target=target,
                platform=platform,
                fmt=fmt,
                sourcemap=sourcemap,
            )
            run_esbuild(esbuild_path, bundle_args)

        if minify:
            bundled_rel = (
                {
                    os.path.relpath(path, ext_dir).replace(os.sep, "/")
                    for path in entrypoints
                }
                if bundle
                else set()
            )
            assets = [
                asset
                for asset in collect_assets(ext_dir)
                if asset not in bundled_rel
            ]
            if assets:
                abs_assets = [
                    os.path.normpath(os.path.join(ext_dir, asset))
                    for asset in assets
                ]
                minify_args = build_esbuild_args(
                    entrypoints=abs_assets,
                    out_dir=out_dir,
                    out_base=ext_dir,
                    minify=True,
                    bundle=False,
                    external=external,
                    target=target,
                    platform=platform,
                    fmt=fmt,
                    sourcemap=sourcemap,
                )
                run_esbuild(esbuild_path, minify_args)

        output_entries.append(
            {
                "name": name,
                "path": rel_path,
                "background_page": bool(ext.get("background_page", False)),
                "enabled_by_default": enabled_by_default,
                "source_root": "gen",
                "source_path": rel_path,
            }
        )

    output = {"extensions": output_entries}

    os.makedirs(os.path.dirname(args.out_json), exist_ok=True)
    with open(args.out_json, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(output, handle, indent=2)
        handle.write("\n")

    write_depfile(args.depfile, [args.out_json], sorted(set(dep_inputs)))

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
