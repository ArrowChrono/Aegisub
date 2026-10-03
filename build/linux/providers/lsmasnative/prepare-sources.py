#!/usr/bin/env python3
"""Validate pinned checkouts and patch private build copies, never the originals."""
import argparse, hashlib, json, os, pathlib, shutil, subprocess
p=argparse.ArgumentParser(); p.add_argument('--sources',type=pathlib.Path,required=True); p.add_argument('--build',type=pathlib.Path); p.add_argument('--verify-only',action='store_true'); a=p.parse_args()
r=pathlib.Path(__file__).resolve().parent; sources=a.sources.resolve(); lock=json.loads((r/'source-lock.json').read_text())
# -C alone does not override inherited GIT_DIR/GIT_WORK_TREE. Neutralize all Git
# process state for source checks as well as patch application.
git_environment={key:value for key,value in os.environ.items() if not key.startswith('GIT_')}
git_environment.update(GIT_CONFIG_NOSYSTEM='1',GIT_CONFIG_GLOBAL=os.devnull)
def verify_patched(name,destination):
    for key,digest in lock['patched_sha256'].items():
        component,relative=key.split('/',1)
        if component==name:
            path=destination/relative
            if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest()!=digest:
                raise SystemExit(f'Patched source hash mismatch: {path}; use a fresh BUILD_ROOT')
for project in lock['projects']:
    tree=sources/project['path']
    actual=subprocess.check_output(['git','-C',str(tree),'rev-parse','HEAD'],text=True,env=git_environment).strip()
    if actual != project['commit']: raise SystemExit(f"Wrong revision: {tree}: {actual}")
    subprocess.run(['git','-C',str(tree),'diff','--quiet','HEAD','--','.'],check=True,env=git_environment)
for name,digest in lock['input_sha256'].items():
    if hashlib.sha256((sources/name).read_bytes()).hexdigest()!=digest: raise SystemExit(f'Wrong source hash: {name}')
print('All six source pins, tracked cleanliness and release patch/header hashes verified')
if a.verify_only: raise SystemExit(0)
if not a.build: p.error('--build is required unless --verify-only')
build=a.build.resolve()
for protected in (sources,r):
    if build==protected or build.is_relative_to(protected) or protected.is_relative_to(build): raise SystemExit('Build root must be disjoint from source/recipe roots')
up=sources/'LsmasSharp'; lsw=up/'third_party/l-smash-works'; prepared=build/'prepared'; prepared.mkdir(parents=True,exist_ok=True)
identity=hashlib.sha256((r/'source-lock.json').read_bytes()).hexdigest()
for name in ('hoe','ffmpeg','zlib'):
    destination=prepared/name; marker=prepared/(name+'.lock')
    if destination.exists():
        if not marker.is_file() or marker.read_text().strip()!=identity: raise SystemExit(f'Refusing stale/unmarked prepared tree: {destination}; use a fresh BUILD_ROOT')
        verify_patched(name,destination)
        continue
    if name=='zlib':
        shutil.copytree(sources/'zlib',destination,ignore=shutil.ignore_patterns('.git'))
        marker.write_text(identity+'\n')
        continue
    if name=='hoe':
        for part in ('common','xxHash'): shutil.copytree(lsw/part,destination/part,ignore=shutil.ignore_patterns('.git'))
        for path in destination.rglob('*'):
            if path.is_file() and path.suffix in ('.c','.h'): path.write_bytes(path.read_bytes().replace(b'\r\n',b'\n'))
        command=['git','-c','core.autocrlf=false','apply','--unidiff-zero',str(up/'src/LsmasNative/patches/hoe-audio-read-contract.patch')]
    else:
        shutil.copytree(lsw/'FFmpeg',destination,ignore=shutil.ignore_patterns('.git'))
        command=['git','-c','core.autocrlf=false','apply',str(up/'src/LsmasNative/patches/ffmpeg-mov-audio-end.patch')]
    # Plain copied trees may live beneath the Aegisub Git worktree. Stop Git
    # before it discovers that enclosing repository; otherwise apply can skip
    # paths successfully because of its inferred prefix.
    patch_environment={**git_environment,'GIT_CEILING_DIRECTORIES':str(destination.parent)}
    subprocess.run(command,cwd=destination,check=True,env=patch_environment)
    verify_patched(name,destination)
    if name=='ffmpeg': (destination/'VERSION').write_text('N-125112-g39c24f3624\n')
    marker.write_text(identity+'\n')
print('Private patched source copies prepared')
