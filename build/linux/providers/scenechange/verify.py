#!/usr/bin/env python3
"""Run standalone ABI/behavior tests, optionally against existing provider files."""
import argparse
import os
from pathlib import Path
import sys

from recipe_common import HERE, build_root, run, source_root, tool


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wwxd-lib", type=Path)
    parser.add_argument("--xvid-lib", type=Path)
    parser.add_argument("--full-xvid-lib", type=Path)
    parser.add_argument("--parity", action="store_true", help="also compare optimized and full Xvid")
    args = parser.parse_args()
    source = source_root()
    root = build_root(source)
    wwxd = (args.wwxd_lib or root / "output/libscenechange_wwxd.so").resolve()
    xvid = (args.xvid_lib or root / "output/libscenechange_xvid.so").resolve()
    full = (args.full_xvid_lib or root / "build/full/libscenechange_xvid.so").resolve()
    for library in [wwxd, xvid, *([full] if args.parity else [])]:
        if not library.is_file():
            parser.error(f"Missing provider library: {library}")
    tests = root / "tests"
    tests.mkdir(parents=True, exist_ok=True)
    environment = os.environ.copy()
    environment.pop("LD_LIBRARY_PATH", None)
    environment.update({"DOTNET_ROOT": "/nonexistent", "DOTNET_ROOT_X64": "/nonexistent",
                        "PYTHONDONTWRITEBYTECODE": "1"})
    run([sys.executable, HERE / "tests/smoke-providers.py", "--source-root", source, wwxd, xvid], env=environment)
    cxx = tool("CXX", "clang++-19")
    for name in ("provider_abi_smoke_linux", "provider_behavior_linux"):
        run([cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", source / "src/native/abi",
             HERE / "tests" / (name + ".cpp"), "-ldl", "-o", tests / name])
        run([tests / name, wwxd], env=environment)
    if args.parity:
        run([sys.executable, HERE / "tests/parity-xvid.py", "--full", full, "--optimized", xvid], env=environment)
    for library in (wwxd, xvid):
        run(["nm", "-D", "--defined-only", library])
        run(["ldd", library], env=environment)
        run(["readelf", "-V", library])
    print("Standalone provider checks passed; application integration and the full managed suite are separate checks.")


if __name__ == "__main__":
    main()
