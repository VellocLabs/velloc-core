#!/usr/bin/env python3
import ctypes
import os
import subprocess
import sys
from typing import Optional

def _create_symlink(source_path: str, target_path: str, *, target_is_directory: bool = True):
    os.makedirs(os.path.dirname(target_path), exist_ok=True)
    source = os.path.abspath(source_path)
    target = os.path.abspath(target_path)
    try:
        print(f"Symlinking {source} to {target}")
        os.symlink(source, target, target_is_directory=target_is_directory)
    except OSError as err:
        if getattr(err, "winerror", None) == 1314:  # ERROR_PRIVILEGE_NOT_HELD
            print("Error: Failed to create symlink, prompting UAC")
            params = (
                f'/c mklink /D "{target}" "{source}"'
                if target_is_directory
                else f'/c mklink "{target}" "{source}"'
            )
            ret = ctypes.windll.shell32.ShellExecuteW(None, "runas", "cmd.exe", params, None, 1)
            if ret <= 32:
                print("Error: Failed to elevate process")
                sys.exit(1)
            print("Successfully created symlink")
        else:
            raise


def _set_skip_worktree(target_file: str, src_root: Optional[str]):
    if not src_root:
        return
    rel_path = os.path.relpath(target_file, src_root)
    if rel_path.startswith(".."):
        return
    try:
        subprocess.run(
            ["git", "update-index", "--skip-worktree", rel_path],
            cwd=src_root,
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    except OSError:
        pass


def _link_file(source_file: str, target_file: str, *, src_root: Optional[str] = None):
    if os.path.lexists(target_file):
        # Avoid re-linking when target already resolves to the override source.
        if os.path.realpath(target_file) == os.path.realpath(source_file):
            _set_skip_worktree(target_file, src_root)
            return
        if os.path.isdir(target_file) and not os.path.islink(target_file):
            print(f"Skipping override for '{target_file}'; a directory already exists there.")
            return
        os.unlink(target_file)
    _create_symlink(source_file, target_file, target_is_directory=False)
    _set_skip_worktree(target_file, src_root)



def _link_directory_tree(source_dir: str, override_root: str, target_root: str):
    rel = os.path.relpath(source_dir, override_root)
    target_dir = target_root if rel == "." else os.path.join(target_root, rel)
    if os.path.lexists(target_dir):
        if os.path.islink(target_dir):
            # Skip directories already symlinked to the override source.
            if os.path.realpath(target_dir) == os.path.realpath(source_dir):
                return
            # Stale or mismatched symlink — remove and re-link.
            os.unlink(target_dir)
            _create_symlink(source_dir, target_dir)
            return
        if not os.path.isdir(target_dir):
            print(f"Skipping override for '{target_dir}'; an existing file is blocking the directory.")
            return
        for entry in sorted(os.listdir(source_dir)):
            child = os.path.join(source_dir, entry)
            child_target = os.path.join(target_dir, entry)
            if os.path.isdir(child):
                _link_directory_tree(child, override_root, target_root)
            else:
                _link_file(child, child_target, src_root=target_root)
        return
    _create_symlink(source_dir, target_dir)


def link_override_files(branding_path_component: str = None):
    script_dir = os.path.dirname(os.path.abspath(__file__))
    src_root = os.path.normpath(os.path.join(script_dir, "..", ".."))
    if not branding_path_component or branding_path_component == "chromium":
        return
    override_root = os.path.join(script_dir, branding_path_component, "symlink")
    if not os.path.isdir(override_root):
        print(f"Override directory '{override_root}' does not exist.")
        return

    _link_directory_tree(override_root, override_root, src_root)


def main(argv=None):
    link_override_files(argv[0] if argv else None)
if __name__ == "__main__":
    main(sys.argv[1:])
