#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path


def _read_version(path: Path) -> str:
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, val = line.split("=", 1)
        values[key.strip()] = val.strip()

    def require(key: str) -> str:
        val = values.get(key)
        if not val or not val.isdigit():
            raise SystemExit(f"Missing or invalid {key} in {path}")
        return val

    major = require("CUSTOM_BROWSER_MAJOR")
    minor = require("CUSTOM_BROWSER_MINOR")
    build = require("CUSTOM_BROWSER_BUILD")
    patch = require("CUSTOM_BROWSER_PATCH")
    return f"{major}.{minor}.{build}.{patch}"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version-file", required=True)
    parser.add_argument("--input-manifest", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--stamp", required=True)
    args = parser.parse_args()

    version = _read_version(Path(args.version_file))
    input_manifest = Path(args.input_manifest)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    output_manifest = output_dir / f"{version}.manifest"
    output_manifest.write_bytes(input_manifest.read_bytes())

    Path(args.stamp).write_text("", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
