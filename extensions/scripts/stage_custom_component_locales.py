#!/usr/bin/env python3
import argparse
import json
import os
import shutil
import sys
from typing import List


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


def copy_tree(src_dir: str, dest_dir: str) -> None:
    if os.path.exists(dest_dir):
        shutil.rmtree(dest_dir)
    os.makedirs(os.path.dirname(dest_dir), exist_ok=True)
    shutil.copytree(src_dir, dest_dir)


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
    parser.add_argument("--root-dir", required=True)
    parser.add_argument("--root-gen-dir")
    parser.add_argument("--out-resources-dir", required=True)
    parser.add_argument("--stamp", required=True)
    parser.add_argument("--depfile")
    args = parser.parse_args()

    root_dir = os.path.abspath(args.root_dir)
    root_gen_dir = (
        os.path.abspath(args.root_gen_dir)
        if args.root_gen_dir
        else None
    )
    out_resources_dir = os.path.abspath(args.out_resources_dir)

    with open(args.input, "r", encoding="utf-8") as handle:
        data = json.load(handle)

    extensions = data.get("extensions", [])
    if not isinstance(extensions, list):
        print("extensions must be a list", file=sys.stderr)
        return 1

    dep_inputs = [os.path.abspath(args.input)]

    for ext in extensions:
        name = ext.get("name")
        if not name or not isinstance(name, str):
            print("extension entry missing valid name", file=sys.stderr)
            return 1

        raw_path = ext.get("path", f"custom_extensions/{name}")
        if not isinstance(raw_path, str):
            print(f"invalid path for extension {name}", file=sys.stderr)
            return 1

        rel_path = normalize_relative_path(raw_path)

        raw_source_path = ext.get("source_path", rel_path)
        if not isinstance(raw_source_path, str):
            print(f"invalid source_path for extension {name}", file=sys.stderr)
            return 1
        source_path = normalize_relative_path(raw_source_path)

        source_root = ext.get("source_root", "src")
        if source_root == "src":
            base_dir = root_dir
        elif source_root == "gen":
            if not root_gen_dir:
                print(
                    f"source_root=gen requires --root-gen-dir for {name}",
                    file=sys.stderr,
                )
                return 1
            base_dir = root_gen_dir
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

        dep_inputs.extend(collect_files(ext_dir))

        dest_dir = os.path.normpath(os.path.join(out_resources_dir, rel_path))
        copy_tree(ext_dir, dest_dir)

    os.makedirs(os.path.dirname(args.stamp), exist_ok=True)
    with open(args.stamp, "w", encoding="utf-8") as handle:
        handle.write("")

    write_depfile(args.depfile, [args.stamp], sorted(set(dep_inputs)))

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
