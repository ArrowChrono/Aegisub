#!/usr/bin/env python3
"""Publish the pinned WWXD file-based project as a Linux x64 NativeAOT library."""
import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess

from recipe_common import HERE, build_root, export_tracked, run, source_root, tool

SDK = "10.0.401"
RUNTIME = "10.0.12"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jobs", type=int, default=int(os.environ.get("JOBS", "2")))
    parser.add_argument("--inprocess-illink", action="store_true",
                        help="opt in to the restricted named-pipe environment workaround")
    parser.add_argument("--dry-run", action="store_true", help="validate inputs and print publish command without writing")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    repo = source_root()
    root = build_root(repo)
    dotnet, cc, cxx = tool("DOTNET", "dotnet"), tool("CC", "clang-19"), tool("CXX", "clang++-19")
    sdk_list = subprocess.check_output([dotnet, "--list-sdks"], text=True)
    if not any(line.startswith(SDK + " ") for line in sdk_list.splitlines()):
        raise SystemExit(f"The selected DOTNET needs installed SDK {SDK}; found:\n{sdk_list}")
    work = root / "wwxd"
    private_source = work / "source"
    output = work / "output"
    environment = os.environ.copy()
    # Every cache and generated project stays in BUILD_ROOT. A caller may seed
    # the private nuget-packages directory in advance for an offline build.
    directories = {
        "DOTNET_CLI_HOME": work / "cli-home",
        "NUGET_PACKAGES": work / "nuget-packages",
        "NUGET_HTTP_CACHE_PATH": work / "nuget-http-cache",
        "NUGET_SCRATCH": work / "nuget-scratch",
        "XDG_DATA_HOME": work / "xdg-data",
        "XDG_CACHE_HOME": work / "xdg-cache",
        "TMPDIR": work / "tmp",
    }
    environment.update({name: str(path) for name, path in directories.items()})
    environment.update({"DOTNET_ROOT": str(Path(dotnet).resolve().parent),
                        "DOTNET_CLI_TELEMETRY_OPTOUT": "1", "DOTNET_SKIP_FIRST_TIME_EXPERIENCE": "1",
                        "DOTNET_NOLOGO": "1", "DOTNET_PROCESSOR_COUNT": str(args.jobs),
                        "MSBUILDDISABLENODEREUSE": "1", "CC": cc, "CXX": cxx,
                        "PATH": str(work / "toolbin") + os.pathsep + environment.get("PATH", "")})
    command = [dotnet, "publish", private_source / "src/native/wwxd/scenechange_wwxd.cs",
               "-c", "Release", "-r", "linux-x64", "--disable-build-servers",
               "-p:BuildInParallel=false", "-p:UseSharedCompilation=false",
               f"-p:IlcMaxDegreeOfParallelism={args.jobs}", f"-p:RuntimeFrameworkVersion={RUNTIME}",
               "-o", output]
    if args.inprocess_illink:
        command += ["-p:CustomBeforeMicrosoftCommonTargets=" + str(HERE / "msbuild-inprocess-illink.targets"),
                    "-p:SceneChangeInProcessILLink=true",
                    "-p:SceneChangeILLinkTasks=" + str(work / "nuget-packages" /
                    "microsoft.net.illink.tasks" / RUNTIME / "tools/net/ILLink.Tasks.dll")]
    if args.dry_run:
        print(f"Export tracked main-repository inputs from {repo} to {private_source}")
        print(f"Select SDK {SDK} with a private global.json; isolate CLI/cache/temp paths in {work}")
        print(shlex.join(map(str, command)))
        print(f"Copy {output / 'scenechange_wwxd.so'} to {root / 'output/libscenechange_wwxd.so'}")
        return
    for path in [*directories.values(), work / "toolbin", output, root / "output", root / "logs"]:
        path.mkdir(parents=True, exist_ok=True)
    for name, target in (("clang", cc), ("clang++", cxx)):
        link = work / "toolbin" / name
        if link.is_symlink() or link.exists():
            link.unlink()
        link.symlink_to(target)
    export_tracked(repo, private_source)
    (work / "global.json").write_text(json.dumps({"sdk": {"version": SDK, "rollForward": "disable"}}, indent=2) + "\n")
    version = subprocess.check_output([dotnet, "--version"], cwd=work, env=environment, text=True).strip()
    if version != SDK:
        raise SystemExit(f"Expected SDK {SDK}; selected {version}")
    (root / "logs/wwxd-publish-command.txt").write_text(shlex.join(map(str, command)) + "\n")
    run(command, cwd=work, env=environment)
    library = root / "output/libscenechange_wwxd.so"
    shutil.copy2(output / "scenechange_wwxd.so", library)
    print(f"Built {library}", flush=True)


if __name__ == "__main__":
    main()
