#!/usr/bin/env python3
"""Collect notices and build identity from the selected source/build inputs."""
import hashlib,json,os,pathlib,shutil,subprocess
recipe=pathlib.Path(__file__).resolve().parent;sources=pathlib.Path(os.environ['SOURCE_ROOT']);build=pathlib.Path(os.environ['BUILD_ROOT']);out=build/'artifacts';notices=out/'licenses';notices.mkdir(parents=True,exist_ok=True)
up=sources/'LsmasSharp';lsw=up/'third_party/l-smash-works'
inputs=[(up/'LICENSE','LsmasSharp-GPL-3.0-or-later.txt'),(up/'THIRD_PARTY_NOTICES.md','LsmasSharp-THIRD-PARTY-NOTICES.md'),(lsw/'FFmpeg/COPYING.GPLv3','FFmpeg-GPLv3.txt'),(lsw/'FFmpeg/LICENSE.md','FFmpeg-LICENSE.md'),(sources/'dav1d/COPYING','dav1d-COPYING.txt'),(sources/'zlib/LICENSE','zlib-LICENSE.txt'),(lsw/'xxHash/LICENSE','xxHash-LICENSE.txt')]
for source,name in inputs:shutil.copyfile(source,notices/name)
# Compiled HOE implementation sources carry their complete ISC notices inline.
(notices/'L-SMASH-Works-source-notices.txt').write_text('\n\n'.join(str(p.relative_to(lsw))+'\n'+p.read_text(errors='replace').split('*/',1)[0]+'*/' for p in sorted((lsw/'common').glob('*.c')) if '*/' in p.read_text(errors='replace')))
lock=json.loads((recipe/'source-lock.json').read_text());lib=out/'lib/liblsmasnative.so'
manifest={'source_lock':lock,'compiler':subprocess.check_output([os.environ.get('CC','clang-19'),'--version'],text=True),'runtime':'lib/liblsmasnative.so','runtime_sha256':hashlib.sha256(lib.read_bytes()).hexdigest(),'license_configuration':'GPL-3.0-or-later, FFmpeg --enable-gpl --enable-version3','verification':'Run verify.sh for fresh test results; build success alone is not a test pass'}
(out/'build-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
shutil.copyfile(recipe/'source-lock.json',out/'source-lock.json')
