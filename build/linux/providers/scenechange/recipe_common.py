"""Shared pin/path checks for the standalone SceneChangeSharp Linux recipes."""
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
LOCK = json.loads((HERE / "source-revisions.json").read_text())


def run(command, **kwargs):
    command = [str(value) for value in command]
    print(shlex.join(command), flush=True)
    return subprocess.run(command, check=True, **kwargs)


def git(source, *arguments):
    return subprocess.check_output(
        ["git", "-C", str(source), *arguments], text=True,
        env={**os.environ, "GIT_OPTIONAL_LOCKS": "0"}
    ).strip()


def source_root():
    value = os.environ.get("SOURCE_ROOT")
    if not value:
        raise SystemExit("Set SOURCE_ROOT to the prepared SceneChangeSharp v0.1.0 Git checkout")
    source = Path(value).expanduser().resolve()
    if not (source / ".git").exists():
        raise SystemExit(f"Expected a Git checkout, including submodules: {source}")
    expected = {
        source: LOCK["scenechange_commit"],
        source / "vendor/xvidcore": LOCK["xvidcore_commit"],
        source / "vendor/vapoursynth-scxvid": LOCK["vapoursynth_scxvid_commit"],
        source / "vendor/vapoursynth-wwxd": LOCK["vapoursynth_wwxd_commit"],
    }
    for path, commit in expected.items():
        if not (path / ".git").exists() or git(path, "rev-parse", "HEAD") != commit:
            raise SystemExit(f"Source pin mismatch or missing submodule: {path}; expected {commit}")
        # Ignore generated/untracked build files, but never consume modified tracked inputs.
        if git(path, "status", "--porcelain=v1", "--untracked-files=no", "--ignore-submodules=untracked"):
            raise SystemExit(f"Tracked source changes present: {path}")
    if git(source, "rev-parse", "refs/tags/" + LOCK["scenechange_tag"]) != LOCK["scenechange_tag_object"]:
        raise SystemExit("SceneChangeSharp annotated v0.1.0 tag object does not match source-revisions.json")
    return source


def build_root(source):
    value = os.environ.get("BUILD_ROOT")
    if not value:
        raise SystemExit("Set BUILD_ROOT to a dedicated build directory outside SOURCE_ROOT and these recipes")
    root = Path(value).expanduser().resolve()
    for protected in (source, HERE):
        if root == protected or root in protected.parents or protected in root.parents:
            raise SystemExit(f"BUILD_ROOT must not overlap source/recipe trees: {root}")
    return root


def tool(variable, default):
    value = os.environ.get(variable, default)
    path = shutil.which(value)
    if not path:
        raise SystemExit(f"{variable} executable not found: {value}")
    return str(Path(path).absolute())


def reset_private_tree(destination):
    # The caller supplies only a fixed child of the checked BUILD_ROOT. Never
    # follow a pre-existing symlink when removing/repopulating that child.
    if destination.is_symlink():
        raise SystemExit(f"Refusing symlink build directory: {destination}")
    if destination.exists():
        shutil.rmtree(destination)
    destination.mkdir(parents=True)


def export_tracked(source, destination, subdirectory=None):
    """Copy tracked files, never Git metadata or untracked build/cache files."""
    reset_private_tree(destination)
    arguments = ["git", "-C", str(source), "ls-files", "-z"]
    if subdirectory:
        arguments += ["--", subdirectory]
    entries = subprocess.check_output(arguments)
    for entry in entries.decode().split("\0"):
        if not entry:
            continue
        origin = source / entry
        if origin.is_dir():  # Gitlinks are verified above; WWXD does not use them.
            continue
        relative = Path(entry).relative_to(subdirectory) if subdirectory else Path(entry)
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(origin, target)
