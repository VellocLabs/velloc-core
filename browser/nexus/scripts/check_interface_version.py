# Copyright 2026 The Custom Browser Authors. All rights reserved.
"""Ninja-time gate: the C++ and nexus_web bridge interface versions must match.

The bridge interface version is defined once per side:
  - C++: `#define NEXUS_INTERFACE_VERSION <int>` in
    browser/nexus/nexus_interface_version.h
  - JS:  `export const NEXUS_INTERFACE_VERSION = <int>` in
    nexus_web/src/browser/bridge/protocol/interfaceVersion.ts

Release pairing is enforced by URL pinning (the binary appends /v<N>/ to the
guest base URL and the web build checks its own path), which leaves the DEV
flow — a co-built checkout serving http://localhost:5173/ with no version
segment — as the one place drift could slip through. This script closes that
gap at build time: any binary build fails if the sibling nexus_web checkout
declares a different interface version.

Wired as a GN action in browser/nexus/BUILD.gn (`:check_interface_version`).
The TS file is reported via a depfile so ninja re-runs the check when either
side's constant changes. Builds without a nexus_web checkout (isolated CI)
can set the GN arg `skip_nexus_interface_version_check = true`.

See docs/agents/nexus-interface-versioning.md.
"""

import argparse
import os
import re
import sys

HEADER_PATTERN = re.compile(
    r"^#define\s+NEXUS_INTERFACE_VERSION\s+(\d+)\s*$", re.MULTILINE)
TS_PATTERN = re.compile(
    r"^export const NEXUS_INTERFACE_VERSION = (\d+)$", re.MULTILINE)

# Path of the guest-side constant inside a nexus_web checkout.
TS_RELATIVE_PATH = os.path.join(
    "src", "browser", "bridge", "protocol", "interfaceVersion.ts")


def _default_nexus_web_dir():
  """Derives the sibling nexus_web checkout from this script's location.

  Monorepo layout: <root>/src/custom_browser/browser/nexus/scripts/<this file>
  and <root>/nexus_web.
  """
  here = os.path.dirname(os.path.abspath(__file__))
  root = os.path.abspath(os.path.join(here, *[os.pardir] * 5))
  return os.path.join(root, "nexus_web")


def _parse_version(path, pattern, side):
  try:
    with open(path, encoding="utf-8") as f:
      content = f.read()
  except OSError as e:
    return None, f"could not read the {side} constant file {path}: {e}"
  match = pattern.search(content)
  if not match:
    return None, (
        f"could not find the NEXUS_INTERFACE_VERSION declaration in {path}; "
        f"the declaration must stay on one line with a literal integer")
  return int(match.group(1)), None


def _write_outputs(args, extra_dep=None):
  deps = [args.header]
  if extra_dep:
    deps.append(extra_dep)
  # Depfile paths are resolved by ninja relative to the build dir (this
  # script's cwd); keep them in the same form the action received them, and
  # normalize to forward slashes for the depfile grammar.
  with open(args.depfile, "w", encoding="utf-8") as f:
    f.write("{}: {}\n".format(
        args.stamp.replace("\\", "/"),
        " ".join(d.replace("\\", "/") for d in deps)))
  with open(args.stamp, "w", encoding="utf-8") as f:
    f.write("")


def main():
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument("--header", required=True,
                      help="Path to nexus_interface_version.h")
  parser.add_argument("--stamp", required=True, help="Stamp file to write")
  parser.add_argument("--depfile", required=True, help="Depfile to write")
  parser.add_argument("--nexus-web-dir", default="",
                      help="nexus_web checkout dir (default: derived sibling)")
  parser.add_argument("--skip", action="store_true",
                      help="Record a warning and pass without checking")
  args = parser.parse_args()

  if args.skip:
    print("WARNING: nexus interface version check SKIPPED "
          "(skip_nexus_interface_version_check=true); this binary's bridge "
          "contract has not been verified against nexus_web.",
          file=sys.stderr)
    _write_outputs(args)
    return 0

  nexus_web_dir = args.nexus_web_dir or _default_nexus_web_dir()
  ts_path = os.path.join(nexus_web_dir, TS_RELATIVE_PATH)
  if not os.path.isfile(ts_path):
    print(f"nexus interface version check FAILED: no nexus_web checkout at "
          f"{nexus_web_dir} (expected {ts_path}).\n"
          f"Either restore the sibling checkout, point the GN arg "
          f"`nexus_web_dir` at one, or set "
          f"`skip_nexus_interface_version_check = true` to build without "
          f"the guest-runtime pairing guarantee.", file=sys.stderr)
    return 1

  cpp_version, err = _parse_version(args.header, HEADER_PATTERN, "C++")
  if err:
    print(f"nexus interface version check FAILED: {err}", file=sys.stderr)
    return 1
  ts_version, err = _parse_version(ts_path, TS_PATTERN, "nexus_web")
  if err:
    print(f"nexus interface version check FAILED: {err}", file=sys.stderr)
    return 1

  if cpp_version != ts_version:
    print(f"nexus interface version check FAILED: the bridge interface "
          f"versions have diverged.\n"
          f"  C++       v{cpp_version}  ({args.header})\n"
          f"  nexus_web v{ts_version}  ({ts_path})\n"
          f"An interface change must bump BOTH constants in paired commits "
          f"(web side first). See docs/agents/nexus-interface-versioning.md.",
          file=sys.stderr)
    return 1

  _write_outputs(args, extra_dep=ts_path)
  return 0


if __name__ == "__main__":
  sys.exit(main())
