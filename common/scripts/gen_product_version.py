#!/usr/bin/env python3
"""Generates the product version from its three sources of truth.

The product version is composed, not stored anywhere as one string:

    {CUSTOM_BROWSER_MAJOR}.{chromium MAJOR}.{bridge interface version}.{CUSTOM_BROWSER_PATCH}
     e.g.        1        .      149       .          1               .        0

  - CUSTOM_BROWSER_MAJOR / CUSTOM_BROWSER_PATCH: custom_browser/VERSION
    (the only hand-edited fields; bumped by scripts/custom_browser_tag.py)
  - chromium MAJOR: chrome/VERSION (tracks the engine upgrade)
  - bridge interface version: NEXUS_INTERFACE_VERSION in
    browser/nexus/nexus_interface_version.h — the same constant that pins
    the /v<N>/ guest URL, so the app version always names the web contract
    the binary speaks (docs/agents/nexus-interface-versioning.md)

Ordering caution: Windows setup.exe refuses installs whose 4-field version
compares below the installed registry `pv` (HIGHER_VERSION_EXISTS), and the
Omaha server offers only higher versions — so every field must be
monotonic across releases: CUSTOM_BROWSER_MAJOR by policy, chromium MAJOR
by upgrade direction, the interface version by design (build-gate
enforced), PATCH within a fixed prefix. Each field must stay <= 65535
(packed Windows VERSIONINFO words / NSIS VIProductVersion).

Outputs keep the historical four CUSTOM_BROWSER_* keys, so downstream
consumers (RC templates, create_installer_archive.py,
copy_version_assembly_manifest.py) are agnostic to the composition:
CUSTOM_BROWSER_MINOR carries the chromium major and CUSTOM_BROWSER_BUILD
carries the interface version.
"""
from __future__ import annotations

import argparse
import re
from pathlib import Path

_INTERFACE_VERSION_RE = re.compile(
    r"^#define\s+NEXUS_INTERFACE_VERSION\s+(\d+)\s*$", re.MULTILINE)


def _read_version_file(path: Path) -> tuple[str, str]:
    """Reads CUSTOM_BROWSER_MAJOR / CUSTOM_BROWSER_PATCH (or bare
    MAJOR / PATCH) from the key=value custom_browser/VERSION file."""
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

    if "CUSTOM_BROWSER_MAJOR" in values:
        return require("CUSTOM_BROWSER_MAJOR"), require("CUSTOM_BROWSER_PATCH")
    if "MAJOR" in values:
        return require("MAJOR"), require("PATCH")
    raise SystemExit(
        f"Unsupported version file format in {path}; "
        "expected CUSTOM_BROWSER_MAJOR/PATCH or MAJOR/PATCH keys."
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


def _read_interface_version(path: Path) -> str:
    match = _INTERFACE_VERSION_RE.search(path.read_text(encoding="utf-8"))
    if not match:
        raise SystemExit(
            f"Could not find '#define NEXUS_INTERFACE_VERSION <int>' in {path}"
        )
    return match.group(1)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True,
                        help="custom_browser/VERSION (MAJOR + PATCH fields)")
    parser.add_argument("--chromium-version-file", required=True,
                        help="chrome/VERSION (supplies the second field)")
    parser.add_argument("--interface-version-file", required=True,
                        help="nexus_interface_version.h (third field)")
    parser.add_argument("--output-inc")
    parser.add_argument("--output-ver")
    parser.add_argument("--output-custom-version-file")
    args = parser.parse_args()

    if not args.output_inc and not args.output_ver and not args.output_custom_version_file:
        raise SystemExit(
            "At least one of --output-inc, --output-ver, or "
            "--output-custom-version-file is required"
        )

    major, patch = _read_version_file(Path(args.input))
    minor = _read_chromium_major(Path(args.chromium_version_file))
    build = _read_interface_version(Path(args.interface_version_file))
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
