#!/usr/bin/env python3
"""Check the actual ELF exports and forbid accidentally dynamic media libraries."""
import argparse,ctypes,hashlib,json,pathlib,re,subprocess
p=argparse.ArgumentParser();p.add_argument('--library',type=pathlib.Path,required=True);p.add_argument('--header',type=pathlib.Path,required=True);p.add_argument('--aegisub-symbols',type=pathlib.Path);p.add_argument('--report',type=pathlib.Path,required=True);a=p.parse_args()
library=a.library.resolve(); dll=ctypes.CDLL(str(library));header=a.header.read_text()
expected=set(re.findall(r'LSMAS_NATIVE_API\s+[\s\w*]*?\b(lsmas_\w+)\s*\(',header))
actual={line.split()[-1] for line in subprocess.check_output(['nm','-D','--defined-only',str(library)],text=True).splitlines() if line.split()}
assert actual==expected,(actual-expected,expected-actual)
assert dll.lsmas_get_api_version()==0x10100
required=set()
if a.aegisub_symbols:
 required=set(re.findall(r'AGI_LSMAS_(?:REQUIRED|OPTIONAL)\((lsmas_\w+)',a.aegisub_symbols.read_text()))
 assert required<=actual,required-actual
elf=subprocess.check_output(['readelf','-d',str(library)],text=True)
needed=re.findall(r'\(NEEDED\).*\[([^]]+)\]',elf)
assert not re.search(r'lib(?:avcodec|avformat|avutil|swscale|swresample|dav1d|z)\.', ' '.join(needed))
assert 'RPATH' not in elf and 'RUNPATH' not in elf
report={'api_version':'1.1.0','public_exports_verified':len(expected),'aegisub_exports_verified':len(required) if a.aegisub_symbols else 'not-run','needed':needed,'sha256':hashlib.sha256(library.read_bytes()).hexdigest()}
a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
