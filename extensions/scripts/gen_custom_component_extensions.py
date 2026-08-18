#!/usr/bin/env python3
import argparse
import json
import os
import sys
from typing import List

IGNORED_FILENAMES = {
    "BUILD.gn",
    "OWNERS",
    "README.md",
    "Prompt.md",
}

IGNORED_EXTENSIONS = {
    ".gn",
    ".gni",
    ".md",
    ".py",
    ".pyc",
    ".pyo",
    ".ts",
    ".tsx",
    ".map",
}

GRD_HEADER = """<?xml version="1.0" encoding="UTF-8"?>
<grit latest_public_release="0" current_release="1">
  <outputs>
    <output filename="grit/custom_component_extension_resources.h" type="rc_header">
      <emit emit_type="prepend"></emit>
    </output>
    <output filename="grit/custom_component_extension_resources_map.cc" type="resource_file_map_source" />
    <output filename="grit/custom_component_extension_resources_map.h" type="resource_map_header" />
    <output filename="custom_component_extension_resources.pak" type="data_package" />
  </outputs>
  <release seq="1">
    <includes>
"""

GRD_FOOTER = """    </includes>
  </release>
</grit>
"""

EMPTY_PLACEHOLDER_RESOURCE_ID = "IDR_CUSTOM_EXT_INTERNAL_PLACEHOLDER_RESOURCE"
EMPTY_PLACEHOLDER_RESOURCE_PATH = "custom_extensions/_internal_placeholder/empty.txt"


def sanitize_id(value: str) -> str:
    result = []
    for ch in value:
        result.append(ch.upper() if ch.isalnum() else "_")
    sanitized = "".join(result)
    while "__" in sanitized:
        sanitized = sanitized.replace("__", "_")
    return sanitized.strip("_")


def cpp_string_literal(value: str) -> str:
    return json.dumps(value)


def normalize_relative_path(value: str) -> str:
    path = value.replace("\\", "/").strip("/")
    if not path:
        raise ValueError("empty extension path")
    if ".." in path.split("/"):
        raise ValueError(f"invalid path: {value}")
    return path


def should_include_file(filename: str) -> bool:
    base = os.path.basename(filename)
    if base.startswith("."):
        return False
    if base in IGNORED_FILENAMES:
        return False
    ext = os.path.splitext(base)[1].lower()
    if ext in IGNORED_EXTENSIONS:
        return False
    return True


def collect_files(ext_dir: str) -> List[str]:
    files = []
    for root, _, filenames in os.walk(ext_dir):
        filenames.sort()
        for filename in filenames:
            if not should_include_file(filename):
                continue
            full_path = os.path.join(root, filename)
            rel_path = os.path.relpath(full_path, ext_dir)
            rel_path = rel_path.replace(os.sep, "/")
            files.append(rel_path)
    return files


def write_depfile(depfile: str, outputs: List[str], inputs: List[str]) -> None:
    if not depfile:
        return
    output_list = " ".join(outputs)
    input_list = " ".join(inputs)
    with open(depfile, "w", encoding="utf-8") as handle:
        handle.write(f"{output_list}: {input_list}\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--output-grd", required=True)
    parser.add_argument("--output-inc", required=True)
    parser.add_argument("--depfile")
    parser.add_argument("--root-dir", required=True)
    parser.add_argument("--root-gen-dir")
    args = parser.parse_args()

    root_dir = os.path.abspath(args.root_dir)
    root_gen_dir = (
        os.path.abspath(args.root_gen_dir)
        if args.root_gen_dir
        else None
    )
    with open(args.input, "r", encoding="utf-8") as handle:
        data = json.load(handle)

    extensions = data.get("extensions", [])
    if not isinstance(extensions, list):
        print("extensions must be a list", file=sys.stderr)
        return 1

    entries = []
    dep_inputs = [os.path.abspath(args.input)]
    resource_ids = set()

    for ext in sorted(extensions, key=lambda e: e.get("name", "")):
        name = ext.get("name")
        if not name or not isinstance(name, str):
            print("extension entry missing valid name", file=sys.stderr)
            return 1

        # Bundled extensions are opt-in: absent means off.
        enabled_by_default = ext.get("enabled_by_default", False)
        if not isinstance(enabled_by_default, bool):
            print(
                f"invalid enabled_by_default for extension {name}",
                file=sys.stderr,
            )
            return 1

        raw_path = ext.get("path", f"custom_extensions/{name}")
        if not isinstance(raw_path, str):
            print(f"invalid path for extension {name}", file=sys.stderr)
            return 1

        try:
            path = normalize_relative_path(raw_path)
        except ValueError as exc:
            print(str(exc), file=sys.stderr)
            return 1

        raw_source_path = ext.get("source_path", path)
        if not isinstance(raw_source_path, str):
            print(f"invalid source_path for extension {name}", file=sys.stderr)
            return 1

        try:
            source_path = normalize_relative_path(raw_source_path)
        except ValueError as exc:
            print(str(exc), file=sys.stderr)
            return 1

        source_root = ext.get("source_root", "src")
        if source_root == "src":
            base_dir = root_dir
            file_prefix = "${root_src_dir}"
        elif source_root == "gen":
            if not root_gen_dir:
                print(
                    f"source_root=gen requires --root-gen-dir for {name}",
                    file=sys.stderr,
                )
                return 1
            base_dir = root_gen_dir
            file_prefix = "${root_gen_dir}"
        else:
            print(
                f"invalid source_root for extension {name}: {source_root}",
                file=sys.stderr,
            )
            return 1

        ext_dir = os.path.normpath(os.path.join(base_dir, source_path))
        if not os.path.isdir(ext_dir):
            print(f"extension directory not found: {ext_dir}", file=sys.stderr)
            return 1

        files = collect_files(ext_dir)
        if "manifest.json" not in files:
            print(f"manifest.json missing in {ext_dir}", file=sys.stderr)
            return 1

        dep_inputs.extend(os.path.abspath(os.path.join(ext_dir, f)) for f in files)

        entries.append(
            {
                "name": name,
                "path": path,
                "source_path": source_path,
                "file_prefix": file_prefix,
                "files": files,
                "background_page": bool(ext.get("background_page", False)),
                "enabled_by_default": enabled_by_default,
            }
        )

    grd_lines = [GRD_HEADER]
    inc_lines = []
    resource_prefix = "custom_browser/extensions"

    if not entries:
        placeholder_path = os.path.join(
            root_dir, "custom_extensions", "_internal_placeholder", "empty.txt"
        )
        if not os.path.isfile(placeholder_path):
            print(
                f"missing placeholder file: {placeholder_path}", file=sys.stderr
            )
            return 1

        resource_ids.add(EMPTY_PLACEHOLDER_RESOURCE_ID)
        dep_inputs.append(os.path.abspath(placeholder_path))
        file_path = (
            f"${{root_src_dir}}/{resource_prefix}/"
            f"{EMPTY_PLACEHOLDER_RESOURCE_PATH}"
        ).replace("\\", "/")
        grd_lines.append(
            f'      <include name="{EMPTY_PLACEHOLDER_RESOURCE_ID}" '
            f'file="{file_path}" '
            f'resource_path="{EMPTY_PLACEHOLDER_RESOURCE_PATH}" '
            f'use_base_dir="false" type="BINDATA" />\n'
        )

    for entry in entries:
        name_key = sanitize_id(entry["name"])
        if not name_key:
            print(f"invalid name for extension {entry['name']}", file=sys.stderr)
            return 1

        manifest_resource_id = None
        for rel_path in sorted(entry["files"]):
            rel_key = sanitize_id(rel_path)
            resource_id = f"IDR_CUSTOM_EXT_{name_key}_{rel_key}"
            if resource_id in resource_ids:
                print(f"duplicate resource id: {resource_id}", file=sys.stderr)
                return 1
            resource_ids.add(resource_id)

            resource_path = f"{entry['path']}/{rel_path}".replace("\\", "/")
            file_path = (
                f"{entry['file_prefix']}/{resource_prefix}/"
                f"{entry['source_path']}/{rel_path}"
            ).replace("\\", "/")
            grd_lines.append(
                f'      <include name="{resource_id}" '
                f'file="{file_path}" '
                f'resource_path="{resource_path}" '
                f'use_base_dir="false" type="BINDATA" />\n'
            )

            if rel_path == "manifest.json":
                manifest_resource_id = resource_id

        if manifest_resource_id is None:
            print(
                f"manifest.json missing for extension {entry['name']}",
                file=sys.stderr,
            )
            return 1

        background_flag = "true" if entry["background_page"] else "false"
        name_literal = cpp_string_literal(entry["name"])
        enabled_flag = "true" if entry["enabled_by_default"] else "false"
        inc_lines.append(
            f"  {{{name_literal}, {manifest_resource_id}, "
            f'FILE_PATH_LITERAL("{entry["path"]}"), '
            f"{background_flag}, {enabled_flag}}},\n"
        )

    grd_lines.append(GRD_FOOTER)

    os.makedirs(os.path.dirname(args.output_grd), exist_ok=True)
    with open(args.output_grd, "w", encoding="utf-8", newline="\n") as handle:
        handle.writelines(grd_lines)

    os.makedirs(os.path.dirname(args.output_inc), exist_ok=True)
    with open(args.output_inc, "w", encoding="utf-8", newline="\n") as handle:
        handle.writelines(inc_lines)

    dep_inputs = sorted(set(dep_inputs))
    write_depfile(args.depfile, [args.output_grd, args.output_inc], dep_inputs)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
