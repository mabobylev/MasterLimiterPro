#!/usr/bin/env python3
"""Copy a Linux VST3 bundle with per-file atomic replacement and SHA-256 checks.

Usage: install_vst3.py --source BUILT.vst3 --destination INSTALLED.vst3 [--dry-run]

Uses only the Python standard library. Each file is copied to a temporary file
in its destination directory, checked, then installed with os.replace. Replacing
the plugin .so creates a new inode, so a running host that already mapped the
old library can continue using it. Restart the host to load the new version.
The sequence is atomic per file, not as a transaction for the whole bundle; do
not load new plugin instances while an install is in progress.
"""

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import sys
import tempfile


MODULEINFO = Path("Contents/Resources/moduleinfo.json")


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def bundle_files(bundle, require_vst3):
    """Return regular bundle files keyed by relative path; reject symlinks."""
    if bundle.is_symlink():
        raise ValueError(f"bundle must not be a symlink: {bundle}")
    if not bundle.is_dir():
        raise ValueError(f"bundle is not a directory: {bundle}")

    files = {}
    for root, directories, filenames in os.walk(bundle, followlinks=False):
        root_path = Path(root)
        for name in directories:
            path = root_path / name
            if path.is_symlink() or not path.is_dir():
                raise ValueError(f"bundle directory must be a real directory: {path}")
        for name in filenames:
            path = root_path / name
            if path.is_symlink() or not path.is_file():
                raise ValueError(f"bundle file must be a regular file: {path}")
            files[path.relative_to(bundle)] = path

    if require_vst3:
        if MODULEINFO not in files:
            raise ValueError(f"missing VST3 module metadata: {bundle / MODULEINFO}")
        if not any(path.suffix == ".so" for path in files):
            raise ValueError(f"missing Linux plugin .so file in: {bundle}")
    return files


def validated_paths(source_arg, destination_arg):
    source_input = source_arg.expanduser().absolute()
    destination_input = destination_arg.expanduser().absolute()

    if not source_input.name.endswith(".vst3"):
        raise ValueError(f"source bundle name must end in .vst3: {source_input}")
    if not destination_input.name.endswith(".vst3"):
        raise ValueError(f"destination bundle name must end in .vst3: {destination_input}")
    if source_input.is_symlink():
        raise ValueError(f"source bundle must not be a symlink: {source_input}")
    if destination_input.is_symlink():
        raise ValueError(f"destination bundle must not be a symlink: {destination_input}")

    source = source_input.resolve(strict=True)
    destination = destination_input.resolve(strict=False)
    if source == destination:
        raise ValueError("source and destination must be different paths")
    if source in destination.parents or destination in source.parents:
        raise ValueError("source and destination bundles must not be nested")
    if not source.is_dir():
        raise ValueError(f"source is not a directory: {source}")

    parent = destination.parent
    if parent.exists() and not parent.is_dir():
        raise ValueError(f"destination parent is not a directory: {parent}")
    return source, destination


def preflight(source, destination):
    source_files = bundle_files(source, require_vst3=True)
    destination_exists = destination.exists()
    if destination_exists:
        if not destination.is_dir():
            raise ValueError(f"destination exists but is not a directory: {destination}")
        destination_files = bundle_files(destination, require_vst3=False)
        extras = sorted(set(destination_files) - set(source_files), key=str)
        if extras:
            names = ", ".join(str(path) for path in extras)
            raise ValueError(f"destination contains files absent from source: {names}")
    hashes = {relative: sha256(path) for relative, path in source_files.items()}
    return source_files, hashes


def install(source, destination, dry_run):
    source_files, source_hashes = preflight(source, destination)
    ordered_paths = sorted(source_files, key=str)

    if dry_run:
        action = "would verify" if destination.exists() else "would install"
        for relative in ordered_paths:
            print(f"{action}: {relative} sha256={source_hashes[relative]}")
        return

    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.mkdir(exist_ok=True)
    for relative in ordered_paths:
        source_file = source_files[relative]
        destination_file = destination / relative
        destination_file.parent.mkdir(parents=True, exist_ok=True)
        temporary_name = None
        try:
            descriptor, temporary_name = tempfile.mkstemp(
                prefix=f".{destination_file.name}.", suffix=".tmp",
                dir=destination_file.parent)
            os.close(descriptor)
            shutil.copy2(source_file, temporary_name)
            expected_hash = source_hashes[relative]
            if sha256(Path(temporary_name)) != expected_hash:
                raise OSError(f"SHA-256 mismatch in temporary copy: {relative}")
            os.replace(temporary_name, destination_file)
            temporary_name = None
            if sha256(destination_file) != expected_hash:
                raise OSError(f"SHA-256 mismatch after replacement: {relative}")
            print(f"verified: {relative} sha256={expected_hash}")
        finally:
            if temporary_name is not None:
                try:
                    os.unlink(temporary_name)
                except FileNotFoundError:
                    pass


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path,
                        help="built Linux VST3 bundle directory ending in .vst3")
    parser.add_argument("--destination", required=True, type=Path,
                        help="installed VST3 bundle directory ending in .vst3")
    parser.add_argument("--dry-run", action="store_true",
                        help="validate inputs and report hashes without creating or changing files")
    args = parser.parse_args(argv)
    try:
        source, destination = validated_paths(args.source, args.destination)
        install(source, destination, args.dry_run)
    except (OSError, ValueError) as error:
        print(f"install_vst3.py: error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
