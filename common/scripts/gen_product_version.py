#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path


def _parse_dotted_version(version: str) -> tuple[str, str, str, str]:
    parts = version.split(".")
    if len(parts) != 4 or any(not p.isdigit() for p in parts):
        raise SystemExit(
            f"Invalid version '{version}'. Expected MAJOR.MINOR.BUILD.PATCH"
        )
    return tuple(parts)  # type: ignore[return-value]


def _read_version_file(path: Path) -> tuple[str, str, str, str]:
    text = path.read_text(encoding="utf-8").strip()
    if not text:
        raise SystemExit(f"Empty version in {path}")

    if "=" not in text:
        return _parse_dotted_version(text)

    values: dict[str, str] = {}
    for line in text.splitlines():
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

    if "CUSTOM_BROWSER_MAJOR" in values:
        major = require("CUSTOM_BROWSER_MAJOR")
        minor = require("CUSTOM_BROWSER_MINOR")
        build = require("CUSTOM_BROWSER_BUILD")
        patch = require("CUSTOM_BROWSER_PATCH")
        return major, minor, build, patch

    if "MAJOR" in values:
        major = require("MAJOR")
        minor = require("MINOR")
        build = require("BUILD")
        patch = require("PATCH")
        return major, minor, build, patch

    raise SystemExit(
        f"Unsupported version file format in {path}; "
        "expected CUSTOM_BROWSER_* or MAJOR/MINOR/BUILD/PATCH keys."
    )

def _read_chromium_major(path: Path) -> str:
    major = None
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("MAJOR="):
            major = line.split("=", 1)[1].strip()
            break
    if not major or not major.isdigit():
        raise SystemExit(f"Invalid Chromium VERSION file: {path}")
    return major


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--output-inc")
    parser.add_argument("--output-ver")
    parser.add_argument("--output-custom-version-file")
    parser.add_argument("--chromium-version-file")
    args = parser.parse_args()

    if not args.output_inc and not args.output_ver and not args.output_custom_version_file:
        raise SystemExit(
            "At least one of --output-inc, --output-ver, or "
            "--output-custom-version-file is required"
        )

    version_path = Path(args.input)
    major, minor, build, patch = _read_version_file(version_path)
    version = f"{major}.{minor}.{build}.{patch}"
    if args.chromium_version_file:
        major = _read_chromium_major(Path(args.chromium_version_file))
        version = f"{major}.{minor}.{build}.{patch}"

    if args.output_inc:
        out_path = Path(args.output_inc)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(
            "// Generated file. Do not edit.\n"
            f"#define CUSTOM_BROWSER_PRODUCT_VERSION \"{version}\"\n",
            encoding="utf-8",
        )

    if args.output_ver:
        out_path = Path(args.output_ver)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(
            f"MAJOR={major}\n"
            f"MINOR={minor}\n"
            f"BUILD={build}\n"
            f"PATCH={patch}\n",
            encoding="utf-8",
        )

    if args.output_custom_version_file:
        out_path = Path(args.output_custom_version_file)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(
            f"CUSTOM_BROWSER_MAJOR={major}\n"
            f"CUSTOM_BROWSER_MINOR={minor}\n"
            f"CUSTOM_BROWSER_BUILD={build}\n"
            f"CUSTOM_BROWSER_PATCH={patch}\n",
            encoding="utf-8",
        )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
