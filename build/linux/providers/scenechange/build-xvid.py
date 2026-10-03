#!/usr/bin/env python3
"""Linux ELF adaptation of SceneChangeSharp v0.1.0 build_scxvid_native.ps1."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
import re
import shlex

from recipe_common import HERE, build_root, export_tracked, reset_private_tree, run, source_root, tool


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--variant", choices=("optimized", "full"), default="optimized")
    parser.add_argument("--jobs", type=int, default=int(os.environ.get("JOBS", "2")))
    parser.add_argument("--dry-run", action="store_true", help="validate inputs and print commands; do not write or compile")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    repo = source_root()
    root = build_root(repo)
    build = root / "build" / args.variant
    src, obj = build / "xvidcore-patched", build / "obj"
    out = root / "output" if args.variant == "optimized" else build
    cc, cxx, nasm, ar = tool("CC", "clang-19"), tool("CXX", "clang++-19"), tool("NASM", "nasm"), tool("AR", "ar")
    patch_tool = tool("PATCH", "patch")
    patches = ["xvidcore-upstream-9304e6d1-image-setedges.patch", "xvidcore-gmc-negative-shift.patch"]
    if args.variant == "optimized":
        patches += ["xvidcore-scene-detect-only.patch", "xvidcore-avx2-sad-batch.patch", "xvidcore-persistent-smp.patch"]
    pristine = repo / "vendor/xvidcore/xvidcore"
    if args.dry_run:
        print(f"Copy {pristine} to private {src}; apply unchanged upstream patch queue")
    else:
        reset_private_tree(build)
        export_tracked(repo / "vendor/xvidcore", src, "xvidcore")
        obj.mkdir()
        out.mkdir(parents=True, exist_ok=True)
        (root / "logs").mkdir(parents=True, exist_ok=True)
    for patch in patches:
        command = [patch_tool, "-p1", "--batch", "--forward", "-i", repo / "patches" / patch]
        if args.dry_run:
            print(shlex.join(map(str, command)))
        else:
            run(command, cwd=src)
    # The pinned patches do not change sources.inc. The two new optimized C
    # files are added explicitly, as in the verified Windows/Linux recipes.
    sources = (pristine / "build/generic/sources.inc").read_text()

    def block(name, next_name, suffix):
        match = re.search(r"\b" + name + r"\s*=\s*\\(.*?)(?=\n" + next_name + r"\b)", sources, re.S)
        if not match:
            raise SystemExit(f"Missing pinned Xvid source list: {name}")
        return re.findall(r"^\s*([^\s\\]+\." + suffix + r")\s*\\?\s*$", match.group(1), re.M)

    cfiles = block("SRC_GENERIC", "SRC_IA32", "c")
    asmfiles = block("SRC_IA32", "SRC_X86_64", "asm")
    if args.variant == "optimized":
        cfiles += ["scxvid_smp_pool.c", "motion/sad_avx2.c"]
    flags = ["-O3", "-std=c11", "-Werror=shift-negative-value", "-fPIC", "-fvisibility=hidden",
             "-D_GNU_SOURCE", "-DARCH_IS_LITTLE_ENDIAN", "-DARCH_IS_64BIT", "-DARCH_IS_X86_64",
             "-DNDEBUG", "-DHAVE_PTHREAD", "-pthread", "-I" + str(src / "src")]
    if args.variant == "optimized":
        flags += ["-DSCXVID_DETECTOR_ONLY", "-DSCXVID_PERSISTENT_SMP", "-DSCXVID_AVX2_SAD_BATCH"]
    commands, objects = [], []
    for source in cfiles:
        output = obj / (source.replace("/", "_") + ".o")
        objects.append(output)
        commands.append([cc, *flags, *(["-mavx2"] if source == "motion/sad_avx2.c" else []),
                         "-c", src / "src" / source, "-o", output])
    for source in asmfiles:
        output = obj / (source.replace("/", "_") + ".o")
        objects.append(output)
        full = src / "src" / source
        commands.append([nasm, "-f", "elf64", "-DMARK_FUNCS", "-DARCH_IS_X86_64",
                         "-I" + str(full.parent) + "/", "-I" + str(src / "src") + "/", "-o", output, full])
    archive = build / "libxvidcore-private.a"
    archive_command = [ar, "rcs", archive, *objects]
    library = out / "libscenechange_xvid.so"
    link = [cxx, "-shared", "-O3", "-std=c++20", "-fPIC", "-fvisibility=hidden", "-DSCXVID_BUILD",
            "-I" + str(src / "src"), "-I" + str(repo / "src/native/xvid")]
    if args.variant == "optimized":
        link.append("-DSCXVID_DETECTOR_ONLY")
    link += [repo / "src/native/xvid/scxvid_bridge.cpp", archive, "-static-libstdc++", "-static-libgcc",
             "-pthread", "-lm", "-Wl,--exclude-libs,ALL", "-Wl,--version-script=" + str(HERE / "scenechange.exports"),
             "-Wl,-z,defs", "-Wl,-z,noexecstack", "-Wl,-soname,libscenechange_xvid.so", "-o", library]
    if args.dry_run:
        for command in [*commands, archive_command, link]:
            print(shlex.join(map(str, command)))
        return
    (root / "logs" / f"xvid-{args.variant}-commands.json").write_text(
        json.dumps([[str(value) for value in command] for command in commands], indent=2) + "\n")
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        list(pool.map(run, commands))
    run(archive_command)
    run(link)
    (root / "logs" / f"xvid-{args.variant}-link-command.txt").write_text(shlex.join(map(str, link)) + "\n")
    print(f"Built {library}", flush=True)


if __name__ == "__main__":
    main()
