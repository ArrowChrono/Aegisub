#!/usr/bin/env python3
"""Stage separately built Linux providers into an existing application layout.

This copies only the explicit dlopen roots. It does not build providers, copy
licenses, resolve their shared-library closure, or create a distributable archive.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys


def relative_path(value: str) -> Path:
    path = Path(value)
    if path.is_absolute() or ".." in path.parts or path == Path("."):
        raise ValueError(f"Expected a nonempty relative path: {value}")
    return path


def plan(base: Path, output: Path | None, manifest: dict) -> list:
    if manifest.get("schema_version") != 1:
        raise ValueError("Unsupported runtime manifest schema")
    result, destinations = [], set()
    for entry in manifest["runtimes"]:
        source = base / relative_path(entry["source"])
        with source.open("rb") as stream:
            header = stream.read(20)
        # ELF64, little-endian, ET_DYN, x86-64; never execute inputs to inspect them.
        if (header[:6] != b"\x7fELF\x02\x01" or len(header) < 20
                or int.from_bytes(header[16:18], "little") != 3
                or int.from_bytes(header[18:20], "little") != 62):
            raise ValueError(f"Not an x86-64 ELF shared library: {source}")
        destination = relative_path(entry["destination"])
        aliases = [relative_path(value) for value in entry.get("aliases", [])]
        for path in [destination, *aliases]:
            if path.parent != Path("bin/runtimes"):
                raise ValueError(f"Provider must be staged directly in bin/runtimes: {path}")
            if path in destinations:
                raise ValueError(f"Duplicate runtime destination: {path}")
            destinations.add(path)
            if output:
                target = output / path
                if not target.resolve().is_relative_to(output):
                    raise ValueError(f"Runtime destination escapes output: {target}")
                if target.exists() or target.is_symlink():
                    raise FileExistsError(f"Refusing to overwrite runtime: {target}")
        result.append((source, destination, aliases))
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", required=True, type=Path,
                        help="Prepared dependency workspace used by runtimes.json")
    parser.add_argument("--manifest", type=Path,
                        default=Path(__file__).with_name("runtimes.json"))
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--validate", action="store_true", help="Read-only input check")
    mode.add_argument("--output", type=Path, help="Application root containing bin/aegisub")
    args = parser.parse_args()
    base = args.base.resolve()
    output = args.output.resolve() if args.output else None
    entries = plan(base, output, json.loads(args.manifest.read_text()))
    records = []
    for source, destination, aliases in entries:
        if output:
            target = output / destination
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
            for alias in aliases:
                (output / alias).symlink_to(destination.name)
        with source.open("rb") as stream:
            checksum = hashlib.file_digest(stream, "sha256").hexdigest()
        records.append({"source": str(source.relative_to(base)),
                        "destination": str(destination),
                        "aliases": [str(alias) for alias in aliases],
                        "sha256": checksum})
    print(json.dumps({"mode": "staged" if output else "validated", "runtimes": records}, indent=2))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError) as error:
        print(f"Runtime staging failed: {error}", file=sys.stderr)
        sys.exit(1)
